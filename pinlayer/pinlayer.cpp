// Holds one layer on layer stack 0 until killed, to measure the smallest layer that keeps
// the mini-HDMI mirror alive. The composer takes the mirror path only while both displays'
// visible-layer lists match, and an app whose list collapses to a single layer loses it.
//
// Usage: xdplus_pinlayer [width] [height] [alpha]

#define LOG_TAG "xdplus-pinlayer"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <log/log.h>
#include <ui/PixelFormat.h>

using namespace android;

int main(int argc, char** argv) {
	const uint32_t width = argc > 1 ? strtoul(argv[1], nullptr, 10) : 1;
	const uint32_t height = argc > 2 ? strtoul(argv[2], nullptr, 10) : 1;
	const float alpha = argc > 3 ? strtof(argv[3], nullptr) : 0.004f;

	sp<SurfaceComposerClient> client = new SurfaceComposerClient();
	if (client->initCheck() != NO_ERROR) {
		fprintf(stderr, "cannot reach SurfaceFlinger\n");
		return 1;
	}

	// An effect layer is created without a size and takes its bounds from the crop.
	sp<SurfaceControl> surface =
			client->createSurface(String8("xdplus-pinlayer"), 0, 0, PIXEL_FORMAT_RGBA_8888,
								  ISurfaceComposerClient::eFXSurfaceEffect);
	if (surface == nullptr || !surface->isValid()) {
		fprintf(stderr, "cannot create the layer\n");
		return 1;
	}

	SurfaceComposerClient::Transaction()
			.setLayerStack(surface, 0)
			.setLayer(surface, 0x7ffffffe)
			.setPosition(surface, 0, 0)
			.setCrop_legacy(surface, Rect(0, 0, static_cast<int32_t>(width),
										  static_cast<int32_t>(height)))
			.setColor(surface, half3{0.0f, 0.0f, 0.0f})
			.setAlpha(surface, alpha)
			.show(surface)
			.apply(true);

	printf("holding %ux%u alpha=%.4f -- kill to release\n", width, height, alpha);
	fflush(stdout);
	pause();
	return 0;
}
