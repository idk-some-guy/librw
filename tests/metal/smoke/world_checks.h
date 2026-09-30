int RunWorldChecks(rw::Camera *camera);
int RunSkinChecks(rw::Camera *camera);
int RunRestartWorldChecks(bool (*restart)(void), rw::Camera *(*camera)(void));
int RunMatFXChecks(rw::Camera *camera);

void WorldOpen(rw::Camera *camera);
void WorldClose(void);
void WorldBegin(rw::RGBA col);
void WorldBeginMode(rw::RGBA col, rw::uint32 clearMode);
void WorldEnd(void);
rw::World *TestWorld(void);
rw::Image *ReadCamera(void);
bool Near(rw::Image *img, int x, int y, rw::RGBA want, int tol);
rw::Texture *MakeTexture(int w, int h, const rw::RGBA *texels, rw::int32 filter, rw::int32 addressU, rw::int32 addressV);
rw::Geometry *QuadGeometry(rw::uint32 flags, float x0, float y0, float x1, float y1, float z,
                           const rw::RGBA col[4], const rw::TexCoords uv[4], rw::Material *mat);
rw::Geometry *SplitQuadGeometry(rw::uint32 flags, float x0, float y0, float x1, float y1, float z,
                                const rw::RGBA col[4], const rw::TexCoords uv[4], rw::Material *mat0, rw::Material *mat1);
rw::Atomic *MakeAtomic(rw::Geometry *geo);
void DestroyAtomic(rw::Atomic *atomic);
rw::Skin *AttachSkin(rw::Geometry *geo, rw::int32 numBones, const rw::Matrix *invBind,
                     const rw::uint8 (*indices)[4], const float (*weights)[4]);
rw::Atomic *SkinAtomic(rw::Geometry *geo);
void DestroySkinAtomic(rw::Atomic *atomic);
