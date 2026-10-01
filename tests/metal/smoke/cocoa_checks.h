struct CocoaWindowInfo
{
	int shown, key, miniaturized, occlusionVisible, accessory;
	long number;
};

struct CocoaLayerInfo
{
	int hostView, metalLayer, deviceLayer, gravityAspect, backgroundBlack;
	int drawableWidth, drawableHeight;
	double contentsScale;
};

void CocoaPumpEvents(void);
void CocoaWindowSize(void *window, int *width, int *height);
void CocoaFramebufferSize(void *window, int *width, int *height);
void CocoaSetWindowSize(void *window, int width, int height);
float CocoaContentScale(void *window);
int CocoaWindowShown(void *window);
bool CocoaWindowInfoOf(void *window, CocoaWindowInfo *info);
bool CocoaLayerInfoOf(void *window, CocoaLayerInfo *info);
bool CocoaServerWindow(long number, int *onScreen);
bool CocoaCurrentDisplayPixels(int *width, int *height);
bool CocoaNativeDisplayPixels(int *width, int *height);
bool CocoaScreenPixels(int *width, int *height);
