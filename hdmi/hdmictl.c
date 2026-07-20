/* hdmictl — manual MTK HDMI bringup helper for GPD XD+ (MT8173, kernel 3.18)
 *
 * The vendor HWC blob never sends MTK_HDMI_VIDEO_CONFIG, so the kernel
 * never raises the res_hdmi switch that triggers the blob's external
 * display bringup. This tool issues the missing ioctls on /dev/hdmitx.
 *
 * usage: hdmictl enable | disable | res <n> | status
 *   res values: 2=720p60 0xb=1080p60 6=1080p30 0=480p60
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define MTK_HDMI_AUDIO_VIDEO_ENABLE	_IO('H', 1)
#define MTK_HDMI_VIDEO_ENABLE		_IO('H', 3)
#define MTK_HDMI_GET_CAPABILITY		_IOWR('H', 4, int)
#define MTK_HDMI_VIDEO_CONFIG		_IOWR('H', 6, int)
#define MTK_HDMI_POWER_ENABLE		_IOW('H', 12, int)
#define MTK_HDMI_FACTORY_GET_STATUS	_IOWR('H', 31, int)
#define MTK_HDMI_FACTORY_DPI_TEST	_IOWR('H', 32, int)

struct mtk_dispif_info {
	unsigned int display_id;
	unsigned int isHwVsyncAvailable;
	unsigned int displayType;
	unsigned int displayWidth;
	unsigned int displayHeight;
	unsigned int displayFormat;
	unsigned int displayMode;
	unsigned int vsyncFPS;
	unsigned int physicalWidth;
	unsigned int physicalHeight;
	unsigned int isConnected;
	unsigned int lcmOriginalWidth;
	unsigned int lcmOriginalHeight;
};
#define MTK_HDMI_GET_DEV_INFO		_IOWR('H', 35, struct mtk_dispif_info)

int main(int argc, char **argv)
{
	int fd, r;

	if (argc < 2) {
		fprintf(stderr, "usage: %s enable|disable|res <n>|status\n", argv[0]);
		return 2;
	}

	fd = open("/dev/hdmitx", O_RDWR);
	if (fd < 0) {
		perror("open /dev/hdmitx");
		return 1;
	}

	if (!strcmp(argv[1], "enable")) {
		r = ioctl(fd, MTK_HDMI_AUDIO_VIDEO_ENABLE, 1);
		printf("AUDIO_VIDEO_ENABLE(1) = %d\n", r);
	} else if (!strcmp(argv[1], "disable")) {
		r = ioctl(fd, MTK_HDMI_AUDIO_VIDEO_ENABLE, 0);
		printf("AUDIO_VIDEO_ENABLE(0) = %d\n", r);
	} else if (!strcmp(argv[1], "power") && argc == 3) {
		r = ioctl(fd, MTK_HDMI_POWER_ENABLE, atoi(argv[2]));
		printf("POWER_ENABLE(%d) = %d\n", atoi(argv[2]), r);
	} else if (!strcmp(argv[1], "res") && argc == 3) {
		unsigned long v = strtoul(argv[2], NULL, 0);
		r = ioctl(fd, MTK_HDMI_VIDEO_CONFIG, v);
		printf("VIDEO_CONFIG(%lu) = %d\n", v, r);
	} else if (!strcmp(argv[1], "devinfo")) {
		struct mtk_dispif_info di;
		memset(&di, 0, sizeof(di));
		di.display_id = 1; /* MTKFB_DISPIF_HDMI */
		r = ioctl(fd, MTK_HDMI_GET_DEV_INFO, &di);
		printf("GET_DEV_INFO = %d: %ux%u type=%u hwvsync=%u fps=%u connected=%u\n",
			r, di.displayWidth, di.displayHeight, di.displayType,
			di.isHwVsyncAvailable, di.vsyncFPS, di.isConnected);
	} else if (!strcmp(argv[1], "dpitest")) {
		/* factory colorbar: EnableColorBar(DPI0) + ddp_dpi_start +
		 * hdmi_video_config — full physical-path probe, no SF/blob */
		int v = 0;
		r = ioctl(fd, MTK_HDMI_FACTORY_DPI_TEST, &v);
		printf("FACTORY_DPI_TEST = %d\n", r);
	} else if (!strcmp(argv[1], "status")) {
		int st = 0;
		r = ioctl(fd, MTK_HDMI_FACTORY_GET_STATUS, &st);
		printf("FACTORY_GET_STATUS = %d, status=%d\n", r, st);
		int cap = 0;
		r = ioctl(fd, MTK_HDMI_GET_CAPABILITY, &cap);
		printf("GET_CAPABILITY = %d, cap=0x%x\n", r, cap);
	} else {
		fprintf(stderr, "bad args\n");
		close(fd);
		return 2;
	}
	if (r < 0)
		perror("ioctl");
	close(fd);
	return r < 0;
}
