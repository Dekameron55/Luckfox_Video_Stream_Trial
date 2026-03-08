/******************************************************************************
*
* Copyright (C) 2025
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
******************************************************************************/

/**
* @file rv1106_jpeg_stream_trial.c
* @brief This is the main application file for the RV1106 JPEG Stream Trial.
* It initializes the camera system (VI, VENC, ISP), captures video frames,
* encodes them as MJPEG, and streams the data to a remote STM32 device via a TCP socket.
*/

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifdef RV1126_RV1109
#include <rk_aiq_user_api_camgroup.h>
#include <rk_aiq_user_api_imgproc.h>
#include <rk_aiq_user_api_sysctl.h>
#else
#include <rk_aiq_user_api2_camgroup.h>
#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>
#endif

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"

#define STM32_IP   "192.168.100.10"
#define STM32_PORT 8080
#define TARGET_FPS 35

#define MAX_AIQ_CTX 8
static rk_aiq_sys_ctx_t *g_aiq_ctx[MAX_AIQ_CTX];
rk_aiq_working_mode_t g_WDRMode[MAX_AIQ_CTX];
static atomic_int g_sof_cnt = 0;
static atomic_bool g_should_quit = false;

typedef struct _rkTestVencCfg {
    RK_BOOL bOutDebugCfg;
    VENC_CHN_ATTR_S stAttr;
    RK_S32 s32ChnId;
} TEST_VENC_CFG;

static RK_S32 g_s32FrameCnt = -1;
static atomic_bool g_quit = false;
static VI_CHN_ATTR_S g_vi_chn_attr;
static TEST_VENC_CFG g_venc_cfgs[1];
static VENC_STREAM_S g_venc_stFrame[1];

static void sigterm_handler(int sig) {
    fprintf(stderr, "signal %d\n", sig);
    g_quit = true;
}

static int g_sock_fd = -1;

int connect_to_stm32() {
    struct sockaddr_in addr;
    if ((g_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;

    // Set a connection timeout
    struct timeval tv_connect = {2, 0}; // 2 seconds
    setsockopt(g_sock_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv_connect, sizeof(tv_connect));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(STM32_PORT);
    inet_pton(AF_INET, STM32_IP, &addr.sin_addr);

    if (connect(g_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
        return -1;
    }

    // Set a send timeout
    struct timeval tv_send = {2, 0}; // 2 seconds
    setsockopt(g_sock_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv_send, sizeof(tv_send));

    return 0;
}

static RK_S32 send_all(int sock, const void *data, size_t size) {
    const char *ptr = (const char*) data;
    size_t sent = 0;
    while (sent < size) {
        ssize_t res = send(sock, ptr + sent, size - sent, MSG_NOSIGNAL);
        if (res <= 0) {
            // Error or connection closed
            if (errno == EPIPE || errno == ECONNRESET) {
                RK_LOGE("Connection lost.");
            } else {
                RK_LOGE("send() failed: %s", strerror(errno));
            }
            return -1;
        }
        sent += res;
    }
    return 0;
}

static XCamReturn SIMPLE_COMM_ISP_SofCb(rk_aiq_metas_t *meta) {
	g_sof_cnt++;
	return XCAM_RETURN_NO_ERROR;
}

static XCamReturn SIMPLE_COMM_ISP_ErrCb(rk_aiq_err_msg_t *msg) {
	if (msg->err_code == XCAM_RETURN_BYPASS) g_should_quit = true;
	return XCAM_RETURN_NO_ERROR;
}

static int vi_dev_init() {
    int ret = 0;
    int devId = 0;
    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    
    ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
    }
    
    if (RK_MPI_VI_GetDevIsEnable(devId) != RK_SUCCESS) {
        RK_MPI_VI_EnableDev(devId);
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = 0;
        RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
    }
    return 0;
}

static int vi_chn_init(int channelId, int width, int height) {
    memset(&g_vi_chn_attr, 0, sizeof(g_vi_chn_attr));
    g_vi_chn_attr.stIspOpt.u32BufCount = 3;
    g_vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    g_vi_chn_attr.stSize.u32Width = width;
    g_vi_chn_attr.stSize.u32Height = height;
    g_vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    g_vi_chn_attr.u32Depth = 0;

    RK_MPI_VI_SetChnAttr(0, channelId, &g_vi_chn_attr);
    return RK_MPI_VI_EnableChn(0, channelId);
}

static void *vi_venc_thread(void *arg) {
    RK_S32 s32Ret;
    RK_S32 loopCount = 0;
    void *pData = RK_NULL;

    while (!g_quit) {
        // 1. Ensure Connection
        if (g_sock_fd == -1) {
            if (connect_to_stm32() < 0) {
                RK_LOGE("Could not connect to STM32, retrying in 1s...");
                usleep(1000000);
                continue;
            } else {
                RK_LOGI("Connected to STM32!");
            }
        }

        s32Ret = RK_MPI_VENC_GetStream(g_venc_cfgs[0].s32ChnId, &g_venc_stFrame[0], -1);
        if (s32Ret == RK_SUCCESS) {
            pData = RK_MPI_MB_Handle2VirAddr(g_venc_stFrame[0].pstPack->pMbBlk);
            uint32_t u32Len = g_venc_stFrame[0].pstPack->u32Len;
            uint32_t u32Len_be = htonl(u32Len);

            // 3. Send to STM32 (Header + Data)
            if (send_all(g_sock_fd, &u32Len_be, 4) < 0 ||
                send_all(g_sock_fd, pData, u32Len) < 0) {
                RK_LOGE("Link lost. Will retry connection.");
                close(g_sock_fd);
                g_sock_fd = -1;
            }

            RK_MPI_VENC_ReleaseStream(g_venc_cfgs[0].s32ChnId, &g_venc_stFrame[0]);
            loopCount++;
        }

        if ((g_s32FrameCnt >= 0) && (loopCount >= g_s32FrameCnt)) g_quit = true;

        // FPS Control
        usleep(1000000 / TARGET_FPS);
    }
    return NULL;
}

RK_S32 SIMPLE_COMM_ISP_Init(RK_S32 CamId, rk_aiq_working_mode_t WDRMode, RK_BOOL MultiCam,
                            const char *iq_file_dir) {
	if (CamId >= MAX_AIQ_CTX) {
		printf("%s : CamId is over 3\n", __FUNCTION__);
		return -1;
	}
	setlinebuf(stdout);
	if (iq_file_dir == NULL) {
		g_aiq_ctx[CamId] = NULL;
		return 0;
	}

	g_WDRMode[CamId] = WDRMode;
	char hdr_str[16];
	snprintf(hdr_str, sizeof(hdr_str), "%d", (int)WDRMode);
	setenv("HDR_MODE", hdr_str, 1);

	rk_aiq_sys_ctx_t *aiq_ctx;
	rk_aiq_static_info_t aiq_static_info;
	rk_aiq_uapi2_sysctl_enumStaticMetas(CamId, &aiq_static_info);

	printf("ID: %d, sensor_name is %s, iqfiles is %s\n", CamId,
	       aiq_static_info.sensor_info.sensor_name, iq_file_dir);

	aiq_ctx =
	    rk_aiq_uapi2_sysctl_init(aiq_static_info.sensor_info.sensor_name, iq_file_dir,
	                             SIMPLE_COMM_ISP_ErrCb, SIMPLE_COMM_ISP_SofCb);

	if (MultiCam)
		rk_aiq_uapi2_sysctl_setMulCamConc(aiq_ctx, true);

	g_aiq_ctx[CamId] = aiq_ctx;
	return 0;
}

RK_S32 SIMPLE_COMM_ISP_Run(RK_S32 CamId) {
	if (CamId >= MAX_AIQ_CTX || !g_aiq_ctx[CamId]) {
		return -1;
	}
	if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx[CamId], 0, 0, g_WDRMode[CamId])) {
		printf("rkaiq engine prepare failed !\n");
		g_aiq_ctx[CamId] = NULL;
		return -1;
	}
	if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx[CamId])) {
		printf("rk_aiq_uapi2_sysctl_start  failed\n");
		return -1;
	}
	return 0;
}

RK_S32 SIMPLE_COMM_ISP_Stop(RK_S32 CamId) {
	if (CamId >= MAX_AIQ_CTX || !g_aiq_ctx[CamId]) {
		return -1;
	}
	rk_aiq_uapi2_sysctl_stop(g_aiq_ctx[CamId], false);
	rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx[CamId]);
	g_aiq_ctx[CamId] = NULL;
	return 0;
}

int main(int argc, char *argv[]) {
    RK_U32 u32Width = 480;
    RK_U32 u32Height = 272;
    RK_CODEC_ID_E enCodec = RK_VIDEO_ID_MJPEG;
    char *iq_file_dir = "/etc/iqfiles/";
    int c;

    while ((c = getopt(argc, argv, "a:w:h:C:l:")) != -1) {
        switch (c) {
            case 'a': iq_file_dir = optarg; break;
            case 'w': u32Width = atoi(optarg); break;
            case 'h': u32Height = atoi(optarg); break;
            case 'C': enCodec = (RK_CODEC_ID_E)atoi(optarg); break;
            case 'l': g_s32FrameCnt = atoi(optarg); break;
        }
    }

    signal(SIGINT, sigterm_handler);

    if (iq_file_dir) {
        SIMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, iq_file_dir);
        SIMPLE_COMM_ISP_Run(0);
    }

    RK_MPI_SYS_Init();

    vi_dev_init();
    vi_chn_init(0, u32Width, u32Height);

    // VENC Config
    g_venc_cfgs[0].s32ChnId = 0;
    g_venc_cfgs[0].bOutDebugCfg = RK_TRUE;
    VENC_CHN_ATTR_S *stAttr = &g_venc_cfgs[0].stAttr;
    memset(stAttr, 0, sizeof(VENC_CHN_ATTR_S));
    
    stAttr->stVencAttr.enType = enCodec;
    stAttr->stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    stAttr->stVencAttr.u32PicWidth = u32Width;
    stAttr->stVencAttr.u32PicHeight = u32Height;
    stAttr->stVencAttr.u32VirWidth = u32Width;
    stAttr->stVencAttr.u32VirHeight = u32Height;
    stAttr->stVencAttr.u32StreamBufCnt = 2;
    stAttr->stVencAttr.u32BufSize = u32Width * u32Height;

    RK_MPI_VENC_CreateChn(0, stAttr);
    
    VENC_RECV_PIC_PARAM_S stRecvParam;
    stRecvParam.s32RecvPicNum = g_s32FrameCnt;
    RK_MPI_VENC_StartRecvFrame(0, &stRecvParam);

    // Bind VI to VENC
    MPP_CHN_S stSrcChn = {RK_ID_VI, 0, 0};
    MPP_CHN_S stDestChn = {RK_ID_VENC, 0, 0};
    RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);

    g_venc_stFrame[0].pstPack = (VENC_PACK_S *)(malloc(sizeof(VENC_PACK_S)));

    pthread_t thread;
    pthread_create(&thread, NULL, vi_venc_thread, NULL);

    while (!g_quit) usleep(100000);

    pthread_join(thread, NULL);

    if (g_sock_fd != -1) close(g_sock_fd);

    RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);

    RK_MPI_SYS_Exit();
    if (iq_file_dir) {
        SIMPLE_COMM_ISP_Stop(0);
    }

    return 0;
}
