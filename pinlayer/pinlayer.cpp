// Holds one layer on layer stack 0 while a second display is attached.
//
// The composer takes the mirror path only while both displays' visible-layer lists match.
// An app whose list collapses to a single layer -- a game or a fullscreen SurfaceView --
// loses the mirror until something else is on screen, and the external display then submits
// nothing at all. One extra layer is enough to keep the lists matching; a 1x1 effect layer
// at alpha 1/255 measures as sufficient, while alpha 0 is culled before the composer sees it.
//
// Usage: xdplus_pinlayer [width] [height] [alpha]   hold a layer until killed
//        xdplus_pinlayer --service                  hold one while an external display is up

#define LOG_TAG "xdplus-pinlayer"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <log/log.h>
#include <ui/PixelFormat.h>

using namespace android;

namespace {

constexpr uint32_t PIN_WIDTH = 1;
constexpr uint32_t PIN_HEIGHT = 1;
constexpr float PIN_ALPHA = 0.004f;
constexpr int POLL_SECONDS = 2;

sp<SurfaceControl> createPin(const sp<SurfaceComposerClient>& client, uint32_t width,
							 uint32_t height, float alpha) {
	// An effect layer is created without a size and takes its bounds from the crop.
	sp<SurfaceControl> surface =
			client->createSurface(String8("xdplus-pinlayer"), 0, 0, PIXEL_FORMAT_RGBA_8888,
								  ISurfaceComposerClient::eFXSurfaceEffect);
	if (surface == nullptr || !surface->isValid()) {
		return nullptr;
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
	return surface;
}

bool externalDisplayAttached() {
	return SurfaceComposerClient::getPhysicalDisplayIds().size() > 1;
}

int runService(const sp<SurfaceComposerClient>& client) {
	sp<SurfaceControl> pin;
	for (;;) {
		const bool wanted = property_get_bool("persist.sys.xdplus.mirror_pin", true) &&
				externalDisplayAttached();
		if (wanted && pin == nullptr) {
			pin = createPin(client, PIN_WIDTH, PIN_HEIGHT, PIN_ALPHA);
			ALOGI(pin != nullptr ? "pin layer held" : "cannot create the pin layer");
		} else if (!wanted && pin != nullptr) {
			pin.clear();
			ALOGI("pin layer released");
		}
		sleep(POLL_SECONDS);
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	sp<SurfaceComposerClient> client = new SurfaceComposerClient();
	if (client->initCheck() != NO_ERROR) {
		fprintf(stderr, "cannot reach SurfaceFlinger\n");
		return 1;
	}

	if (argc > 1 && strcmp(argv[1], "--service") == 0) {
		return runService(client);
	}

	const uint32_t width = argc > 1 ? strtoul(argv[1], nullptr, 10) : PIN_WIDTH;
	const uint32_t height = argc > 2 ? strtoul(argv[2], nullptr, 10) : PIN_HEIGHT;
	const float alpha = argc > 3 ? strtof(argv[3], nullptr) : PIN_ALPHA;

	sp<SurfaceControl> pin = createPin(client, width, height, alpha);
	if (pin == nullptr) {
		fprintf(stderr, "cannot create the layer\n");
		return 1;
	}

	printf("holding %ux%u alpha=%.4f -- kill to release\n", width, height, alpha);
	fflush(stdout);
	pause();
	return 0;
}
