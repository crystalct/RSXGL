// RSXGL - Graphics library for the PS3 GPU.
//
// Copyright (c) 2011 Alexander Betts (alex.betts@gmail.com)
//
// framebuffer.cc - Offscreen rendering targets.

#include "rsxgl_context.h"
#include "gl_constants.h"
#include "framebuffer.h"

#include <GL3/gl3.h>
#include "error.h"

#include "gcm.h"
#include "nv40.h"
#include "gl_fifo.h"
#include "cxxutil.h"

#if defined(GLAPI)
#undef GLAPI
#endif
#define GLAPI extern "C"

// Renderbuffers:
renderbuffer_t::storage_type & renderbuffer_t::storage()
{
  static renderbuffer_t::storage_type _storage(0,0);
  return _storage;
}

renderbuffer_t::renderbuffer_t()
  : deleted(0), timestamp(0), ref_count(0), arena(0)
{
}

renderbuffer_t::~renderbuffer_t()
{
  // Free memory used by this buffer:
  if(surface.memory.offset != 0) {
    rsxgl_arena_free(memory_arena_t::storage().at(arena),surface.memory);
  }
}

GLAPI void APIENTRY
glGenRenderbuffers (GLsizei count, GLuint *renderbuffers)
{
  GLsizei n = renderbuffer_t::storage().create_names(count,renderbuffers);

  if(count != n) {
    RSXGL_ERROR_(GL_OUT_OF_MEMORY);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glDeleteRenderbuffers (GLsizei count, const GLuint *renderbuffers)
{
  struct rsxgl_context_t * ctx = current_ctx();

  for(GLsizei i = 0;i < count;++i,++renderbuffers) {
    GLuint renderbuffer_name = *renderbuffers;

    if(renderbuffer_name == 0) continue;

    // Free resources used by this object:
    if(renderbuffer_t::storage().is_object(renderbuffer_name)) {
      ctx -> renderbuffer_binding.unbind_from_all(renderbuffer_name);

      // TODO - orphan it, instead of doing this:
      renderbuffer_t & renderbuffer = renderbuffer_t::storage().at(renderbuffer_name);
      if(renderbuffer.timestamp > 0) {
	rsxgl_timestamp_wait(ctx,renderbuffer.timestamp);
	renderbuffer.timestamp = 0;
      }

      renderbuffer_t::gl_object_type::maybe_delete(renderbuffer_name);
    }
    else if(renderbuffer_t::storage().is_name(renderbuffer_name)) {
      renderbuffer_t::storage().destroy(renderbuffer_name);
    }
  }

  RSXGL_NOERROR_();
}

GLAPI GLboolean APIENTRY
glIsRenderbuffer (GLuint renderbuffer)
{
  return renderbuffer_t::storage().is_object(renderbuffer);
}

static inline uint8_t
rsxgl_renderbuffer_format(GLenum internalformat)
{
  if(internalformat == GL_RGBA || internalformat == GL_RGBA8) {
    return RSXGL_RENDERBUFFER_FORMAT_A8R8G8B8;
  }
  else if(internalformat == GL_RGB || internalformat == GL_RGB8) {
    return RSXGL_RENDERBUFFER_FORMAT_X8R8G8B8;
  }
  else if(internalformat == GL_RED || internalformat == GL_R8) {
    return RSXGL_RENDERBUFFER_FORMAT_B8;
  }
  else if(internalformat == GL_DEPTH_COMPONENT16 || internalformat == GL_DEPTH_COMPONENT) {
    return RSXGL_RENDERBUFFER_FORMAT_DEPTH16;
  }
  else if(internalformat == GL_DEPTH_COMPONENT24) {
    return RSXGL_RENDERBUFFER_FORMAT_DEPTH24_D8;
  }
  else {
    return ~0;
  }
}

static uint8_t
rsxgl_renderbuffer_bytesPerPixel[] = {
  2,
  4,
  4,
  1,
  4 * sizeof(float) / 2,
  4 * sizeof(float),
  sizeof(float),
  4,
  4,
  4,
  2
};

static inline uint32_t
rsxgl_renderbuffer_target(GLenum target)
{
  switch(target) {
  case GL_RENDERBUFFER:
    return RSXGL_RENDERBUFFER;
    break;
  default:
    return ~0;
  };
}

GLAPI void APIENTRY
glBindRenderbuffer (GLuint target, GLuint renderbuffer_name)
{
  const uint32_t rsx_target = rsxgl_renderbuffer_target(target);
  if(rsx_target == ~0) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  if(!(renderbuffer_name == 0 || renderbuffer_t::storage().is_name(renderbuffer_name))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(renderbuffer_name != 0 && !renderbuffer_t::storage().is_object(renderbuffer_name)) {
    renderbuffer_t::storage().create_object(renderbuffer_name);
  }

  rsxgl_context_t * ctx = current_ctx();
  ctx -> renderbuffer_binding.bind(rsx_target,renderbuffer_name);

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glRenderbufferStorage (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
  const uint32_t rsx_target = rsxgl_renderbuffer_target(target);
  if(rsx_target == ~0) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  if(width < 0 || width > RSXGL_MAX_RENDERBUFFER_SIZE || height < 0 || height > RSXGL_MAX_RENDERBUFFER_SIZE) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  uint8_t rsx_format = rsxgl_renderbuffer_format(internalformat);
  if(rsx_format == ~0) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  rsxgl_context_t * ctx = current_ctx();

  if(ctx -> renderbuffer_binding.names[rsx_target] == 0) {
    RSXGL_ERROR_(GL_INVALID_OPERATION);
  }
  renderbuffer_t & renderbuffer = ctx -> renderbuffer_binding[rsx_target];

  // TODO - orphan instead of delete:
  if(renderbuffer.timestamp > 0) {
    rsxgl_timestamp_wait(ctx,renderbuffer.timestamp);
    renderbuffer.timestamp = 0;
  }

  surface_t & surface = renderbuffer.surface;

  if(surface.memory.offset != 0) {
    rsxgl_arena_free(memory_arena_t::storage().at(renderbuffer.arena),surface.memory);
  }

  memory_arena_t::name_type arena = ctx -> arena_binding.names[RSXGL_RENDERBUFFER_ARENA];

  const uint32_t pitch = align_pot< uint32_t, 64 >(width * rsxgl_renderbuffer_bytesPerPixel[rsx_format]);
  const uint32_t nbytes = pitch * height;

  surface.memory = rsxgl_arena_allocate(memory_arena_t::storage().at(arena),128,nbytes);
  if(surface.memory.offset == 0) {
    RSXGL_ERROR_(GL_OUT_OF_MEMORY);
  }

  surface.format = rsx_format;
  surface.size[0] = width;
  surface.size[1] = height;
  surface.pitch = pitch;

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetRenderbufferParameteriv (GLenum target, GLenum pname, GLint *params)
{
  const uint32_t rsx_target = rsxgl_renderbuffer_target(target);
  if(rsx_target == ~0) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  RSXGL_NOERROR_();
}

// Framebuffers:
framebuffer_t::storage_type & framebuffer_t::storage()
{
  static framebuffer_t::storage_type _storage(0,0);
  return _storage;
}

framebuffer_t::framebuffer_t()
{
  for(size_t i = 0;i < RSXGL_MAX_ATTACHMENTS;++i) {
    attachments[i] = 0;
  }
}

framebuffer_t::~framebuffer_t()
{
  for(size_t i = 0;i < RSXGL_MAX_ATTACHMENTS;++i) {
    const uint32_t attachment_type = attachment_types.get(i);
    if(attachment_type == RSXGL_ATTACHMENT_TYPE_NONE) {
      continue;
    }
    else if(attachment_type == RSXGL_ATTACHMENT_TYPE_RENDERBUFFER && attachments[i] != 0) {
      renderbuffer_t::gl_object_type::unref_and_maybe_delete(attachments[i]);
    }
    else if(attachment_type == RSXGL_ATTACHMENT_TYPE_TEXTURE && attachments[i] != 0) {
      texture_t::gl_object_type::unref_and_maybe_delete(attachments[i]);
    }
  }
}

GLAPI void APIENTRY
glGenFramebuffers (GLsizei n, GLuint *framebuffers)
{
  GLsizei count = framebuffer_t::storage().create_names(n,framebuffers);

  if(count != n) {
    RSXGL_ERROR_(GL_OUT_OF_MEMORY);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glDeleteFramebuffers (GLsizei n, const GLuint *framebuffers)
{
  struct rsxgl_context_t * ctx = current_ctx();

  for(GLsizei i = 0;i < n;++i,++framebuffers) {
    GLuint framebuffer_name = *framebuffers;

    if(framebuffer_name == 0) continue;

    if(framebuffer_t::storage().is_object(framebuffer_name)) {
      ctx -> framebuffer_binding.unbind_from_all(framebuffer_name);
      framebuffer_t::storage().destroy(framebuffer_name);
    }
    else if(framebuffer_t::storage().is_name(framebuffer_name)) {
      framebuffer_t::storage().destroy(framebuffer_name);
    }
  }

  RSXGL_NOERROR_();
}

GLAPI GLboolean APIENTRY
glIsFramebuffer (GLuint framebuffer)
{
  return framebuffer_t::storage().is_object(framebuffer);
}

GLAPI void APIENTRY
glBindFramebuffer (GLenum target, GLuint framebuffer_name)
{
  if(!(target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  if(!(framebuffer_name == 0 || framebuffer_t::storage().is_name(framebuffer_name))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(framebuffer_name != 0 && !framebuffer_t::storage().is_object(framebuffer_name)) {
    framebuffer_t::storage().create_object(framebuffer_name);
  }

  rsxgl_context_t * ctx = current_ctx();

  if(target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
    ctx -> framebuffer_binding.bind(RSXGL_DRAW_FRAMEBUFFER,framebuffer_name);
  }
  if(target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
    ctx -> framebuffer_binding.bind(RSXGL_READ_FRAMEBUFFER,framebuffer_name);
  }

  ctx -> state.invalid.parts.draw_framebuffer = 1;

  RSXGL_NOERROR_();
}

static inline uint32_t
rsxgl_framebuffer_target(GLenum target)
{
  switch(target) {
  case GL_FRAMEBUFFER:
  case GL_DRAW_FRAMEBUFFER:
    return RSXGL_DRAW_FRAMEBUFFER;
    break;
  case GL_READ_FRAMEBUFFER:
    return RSXGL_READ_FRAMEBUFFER;
    break;
  default:
    return ~0;
  };
}

static inline uint32_t
rsxgl_framebuffer_attachment(GLenum attachment)
{
  if(attachment >= GL_COLOR_ATTACHMENT0 && attachment < (GL_COLOR_ATTACHMENT0 + RSXGL_MAX_COLOR_ATTACHMENTS)) {
    return RSXGL_COLOR_ATTACHMENT_COLOR0 + (attachment - GL_COLOR_ATTACHMENT0);
  }
  else if(attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
    return RSXGL_DEPTH_STENCIL_ATTACHMENT;
  }
  else {
    return ~0;
  }
}

static inline uint32_t
rsxgl_framebuffer_attachment_type(GLenum target)
{
  if(target == GL_RENDERBUFFER) {
    return RSXGL_ATTACHMENT_TYPE_RENDERBUFFER;
  }
  else {
    return ~0;
  }
}

GLAPI GLenum APIENTRY
glCheckFramebufferStatus (GLenum target)
{
  //RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFramebufferTexture1D (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFramebufferTexture3D (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset)
{
  RSXGL_NOERROR_();
}

static inline void
rsxgl_framebuffer_renderbuffer(rsxgl_context_t * ctx,const framebuffer_t::name_type framebuffer_name,const GLenum attachment, const GLenum renderbuffertarget, const GLuint renderbuffer_name)
{
  const uint32_t rsx_attachment = rsxgl_framebuffer_attachment(attachment);
  if(rsx_attachment == ~0) {
    RSXGL_NOERROR_();
  }

  const uint32_t rsx_attachment_type = rsxgl_framebuffer_attachment_type(renderbuffertarget);
  if(rsx_attachment_type != RSXGL_ATTACHMENT_TYPE_RENDERBUFFER) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  if(renderbuffer_name != 0 && !renderbuffer_t::storage().is_object(renderbuffer_name)) {
    RSXGL_ERROR_(GL_INVALID_OPERATION);
  }

  framebuffer_t & framebuffer = framebuffer_t::storage().at(framebuffer_name);

  // Detach whatever was there:
  if(framebuffer.attachments[rsx_attachment] != 0) {
    const uint32_t attachment_type = framebuffer.attachment_types.get(rsx_attachment);
    if(attachment_type == RSXGL_ATTACHMENT_TYPE_RENDERBUFFER) {
      // TODO - maybe orphan:
      renderbuffer_t::gl_object_type::unref_and_maybe_delete(framebuffer.attachments[rsx_attachment]);
    }
    else if(attachment_type == RSXGL_ATTACHMENT_TYPE_TEXTURE) {
      // TODO - maybe orphan:
      texture_t::gl_object_type::unref_and_maybe_delete(framebuffer.attachments[rsx_attachment]);
    }
    framebuffer.attachment_types.set(rsx_attachment,RSXGL_ATTACHMENT_TYPE_NONE);
    framebuffer.attachments[rsx_attachment] = 0;
  }

  // Attach the renderbuffer to it:
  if(renderbuffer_name != 0) {
    framebuffer.attachment_types.set(rsx_attachment,RSXGL_ATTACHMENT_TYPE_RENDERBUFFER);
    framebuffer.attachments[rsx_attachment] = renderbuffer_name;
  }

  if(ctx -> framebuffer_binding.is_bound(RSXGL_DRAW_FRAMEBUFFER,framebuffer_name)) ctx -> state.invalid.parts.draw_framebuffer = 1;
  if(ctx -> framebuffer_binding.is_bound(RSXGL_READ_FRAMEBUFFER,framebuffer_name)) ctx -> state.invalid.parts.read_framebuffer = 1;

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
  const uint32_t rsx_framebuffer_target = rsxgl_framebuffer_target(target);
  if(rsx_framebuffer_target == ~0) {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  rsxgl_context_t * ctx = current_ctx();
  if(!ctx -> framebuffer_binding.is_anything_bound(rsx_framebuffer_target)) {
    RSXGL_ERROR_(GL_INVALID_OPERATION);
  }

  rsxgl_framebuffer_renderbuffer(ctx,ctx -> framebuffer_binding.names[rsx_framebuffer_target],attachment,renderbuffertarget,renderbuffer);
}

GLAPI void APIENTRY
glGetFramebufferAttachmentParameteriv (GLenum target, GLenum attachment, GLenum pname, GLint *params)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGenerateMipmap (GLenum target)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glBlitFramebuffer (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glRenderbufferStorageMultisample (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFramebufferTextureLayer (GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFramebufferTexture (GLenum target, GLenum attachment, GLuint texture, GLint level)
{
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glDrawBuffer (GLenum mode)
{
}

GLAPI void APIENTRY
glDrawBuffers(GLsizei n, const GLenum *bufs)
{
}

static uint32_t
rsxgl_renderbuffer_nv40_format[] = {
    NV30_3D_RT_FORMAT_COLOR_R5G6B5,
    NV30_3D_RT_FORMAT_COLOR_X8R8G8B8,
    NV30_3D_RT_FORMAT_COLOR_A8R8G8B8,
    NV30_3D_RT_FORMAT_COLOR_B8,
    NV30_3D_RT_FORMAT_COLOR_A16B16G16R16_FLOAT,
    NV30_3D_RT_FORMAT_COLOR_A32B32G32R32_FLOAT,
    NV30_3D_RT_FORMAT_COLOR_R32_FLOAT,
    NV30_3D_RT_FORMAT_COLOR_X8B8G8R8,
    NV30_3D_RT_FORMAT_COLOR_A8B8G8R8,
    NV30_3D_RT_FORMAT_ZETA_Z24S8,
    NV30_3D_RT_FORMAT_ZETA_Z16
};

static const uint32_t
rsxgl_dma_methods[] = {
  NV30_3D_DMA_COLOR0,
  NV30_3D_DMA_COLOR1,
  NV40_3D_DMA_COLOR2,
  NV40_3D_DMA_COLOR3,
  NV30_3D_DMA_ZETA
};

static const uint32_t
rsxgl_offset_methods[] = {
  NV30_3D_COLOR0_OFFSET,
  NV30_3D_COLOR1_OFFSET,
  NV40_3D_COLOR2_OFFSET,
  NV40_3D_COLOR3_OFFSET,
  NV30_3D_ZETA_OFFSET
};

static const uint32_t
rsxgl_pitch_methods[] = {
  NV30_3D_COLOR0_PITCH,
  NV30_3D_COLOR1_PITCH,
  NV40_3D_COLOR2_PITCH,
  NV40_3D_COLOR3_PITCH,
  NV40_3D_ZETA_PITCH
};

static inline void
rsxgl_emit_surface(gcmContextData * context,const uint8_t which,surface_t const & surface)
{
  uint32_t * buffer = gcm_reserve(context,6);
	
  gcm_emit_method_at(buffer,0,rsxgl_dma_methods[which],1);
  gcm_emit_at(buffer,1,(surface.memory.location == RSXGL_MEMORY_LOCATION_LOCAL) ? RSXGL_DMA_MEMORY_FRAME_BUFFER : RSXGL_DMA_MEMORY_HOST_BUFFER);
  
  gcm_emit_method_at(buffer,2,rsxgl_offset_methods[which],1);
  gcm_emit_at(buffer,3,surface.memory.offset);
  
  gcm_emit_method_at(buffer,4,rsxgl_pitch_methods[which],1);
  gcm_emit_at(buffer,5,surface.pitch);
  
  gcm_finish_n_commands(context,6);
}

void
rsxgl_draw_framebuffer_validate(rsxgl_context_t * ctx,const uint32_t timestamp)
{
  gcmContextData * context = ctx -> gcm_context();

  if(ctx -> state.invalid.parts.draw_framebuffer) {
    uint16_t enabled = 0, format = 0, w = 0, h = 0;

    // no FBO is bound - use the window system's:
    if(ctx -> framebuffer_binding.names[RSXGL_DRAW_FRAMEBUFFER] == 0) {
      surface_t const & surface = ctx -> color_surfaces[ctx -> draw_buffer];

      rsxgl_emit_surface(context,0,surface);
      rsxgl_emit_surface(context,4,ctx -> depth_surface);

      format = (uint32_t)ctx -> surfaces_format;
      w = surface.size[0];
      h = surface.size[1];
      enabled = NV30_3D_RT_ENABLE_COLOR0;
    }
    // do something with the attached FBO:
    else {
      
    }

    {
      uint32_t * buffer = gcm_reserve(context,9);
      
      gcm_emit_method_at(buffer,0,NV30_3D_RT_FORMAT,1);
      gcm_emit_at(buffer,1,(uint32_t)format | ((31 - __builtin_clz(w)) << NV30_3D_RT_FORMAT_LOG2_WIDTH__SHIFT) | ((31 - __builtin_clz(h)) << NV30_3D_RT_FORMAT_LOG2_HEIGHT__SHIFT));
      
      gcm_emit_method_at(buffer,2,NV30_3D_RT_HORIZ,2);
      gcm_emit_at(buffer,3,w << 16);
      gcm_emit_at(buffer,4,h << 16);
      
      gcm_emit_method_at(buffer,5,NV30_3D_COORD_CONVENTIONS,1);
      gcm_emit_at(buffer,6,h | NV30_3D_COORD_CONVENTIONS_ORIGIN_NORMAL);
      
      gcm_emit_method_at(buffer,7,NV30_3D_RT_ENABLE,1);
      gcm_emit_at(buffer,8,enabled);
      
      gcm_finish_n_commands(context,9);
    }

    ctx -> state.invalid.parts.draw_framebuffer = 0;
  }
}
