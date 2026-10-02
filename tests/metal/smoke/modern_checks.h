int RunModernChecks(rw::Camera *camera);
int RunRestartModernChecks(bool (*restart)(rw::uint32 samples), rw::Camera *(*camera)(void));
