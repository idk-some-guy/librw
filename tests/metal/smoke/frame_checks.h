int RunFrameChecks(rw::Camera *camera);
int RunRestartFrameChecks(bool (*restart)(rw::uint32 samples), rw::Camera *(*camera)(void));
