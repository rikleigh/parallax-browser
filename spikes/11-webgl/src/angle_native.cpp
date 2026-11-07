// angle_native.cpp - the ps5-opengl name of an ANGLE texture, for the share
// probe. ANGLE gives the page its own object names; the compositor needs the
// native one. Built with ANGLE's own defines and include paths, since it uses
// ANGLE's internal headers (a WebKit port would add an entry point like this).
#include "libANGLE/Context.h"
#include "libANGLE/Texture.h"
#include "libANGLE/renderer/gl/TextureGL.h"
#include "libGLESv2/global_state.h"

extern "C" unsigned angle_ps5_native_texture(unsigned name)
{
    gl::Context *context = gl::GetValidGlobalContext();
    if (!context)
        return 0;
    gl::Texture *texture = context->getTexture(gl::TextureID{name});
    return texture ? rx::GetImplAs<rx::TextureGL>(texture)->getTextureID() : 0;
}
