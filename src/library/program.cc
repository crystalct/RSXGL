// RSXGL - Graphics library for the PS3 GPU.
//
// Copyright (c) 2011 Alexander Betts (alex.betts@gmail.com)
//
// program.cc - Functions pertaining to creating, compiling, and linking shaders and programs.

#include <GL3/gl3.h>

#include "debug.h"
#include "rsxgl_assert.h"
#include "rsxgl_context.h"
#include "error.h"
#include "gl_fifo.h"
#include "program.h"
#include "compiler_context.h"

#include "gcm.h"
#include "nv40.h"

#define MSPACES 1
#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define malloc_getpagesize 4096
#include "malloc-2.8.4.h"

// Get a few things from cgcomp's headers:
#define __TYPES_H__
#define FLOAT32  0x0
#define FLOAT16  0x1
#define FIXED12  0x2
//#define INLINE __inline
#define boolean char
#include "nv30_vertprog.h"
#include "nv40_vertprog.h"
#include "nvfx_shader.h"
#undef FLOAT32
#undef FLOAT16
#undef FIXED12
//#undef INLINE
#undef boolean
#undef __TYPES_H__

// mesa:
#include <main/mtypes.h>
#include <state_tracker/st_program.h>
#include <program/prog_parameter.h>
#include <glsl/ir.h>
#include <glsl/ir_uniform.h>

extern "C" {
#include <nvfx/nvfx_state.h>
}

#include <string.h>
#include <malloc.h>
#include <algorithm>
#include <deque>
#include <map>
#include "set_algorithm2.h"

#if defined(GLAPI)
#undef GLAPI
#endif
#define GLAPI extern "C"

//
shader_t::storage_type & shader_t::storage()
{
  return current_object_ctx() -> shader_storage();
}

program_t::storage_type & program_t::storage()
{
  return current_object_ctx() -> program_storage();
}

// Shader functions:
shader_t::shader_t()
  : type(RSXGL_MAX_SHADER_TYPES), compiled(GL_FALSE), deleted(GL_FALSE), ref_count(0), mesa_shader(0)
{
  source().construct();
  binary().construct();
  info().construct();
}

shader_t::~shader_t()
{
  source().destruct();
  binary().destruct();
  info().destruct();
}

static inline uint32_t 
rsxgl_shader_type(GLenum type)
{
  switch(type) {
  case GL_VERTEX_SHADER:
    return RSXGL_VERTEX_SHADER;
  case GL_FRAGMENT_SHADER:
    return RSXGL_FRAGMENT_SHADER;
  default:
    return RSXGL_MAX_SHADER_TYPES;
  };
}

GLAPI GLuint APIENTRY
glCreateShader (GLenum type)
{
  uint32_t rsx_type = rsxgl_shader_type(type);
  if(rsx_type == RSXGL_MAX_SHADER_TYPES) {
    RSXGL_ERROR(GL_INVALID_ENUM,0);
  }

  uint32_t name = shader_t::storage().create_name_and_object();
  shader_t::storage().at(name).type = rsx_type;

  shader_t & shader = shader_t::storage().at(name);
  compiler_context_t * cctx = current_ctx() -> compiler_context();
  shader.mesa_shader = cctx -> create_shader(rsx_type == RSXGL_VERTEX_SHADER ? compiler_context_t::kVertex : compiler_context_t::kFragment);

  RSXGL_NOERROR(name);
}

GLAPI void APIENTRY
glDeleteShader (GLuint shader_name)
{
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }
  
  shader_t::gl_object_type::maybe_delete(shader_name);

  // TODO: destroy mesa resources

  RSXGL_NOERROR_();
}

GLAPI GLboolean APIENTRY
glIsShader (GLuint shader_name)
{
  RSXGL_NOERROR((shader_t::storage().is_object(shader_name)) ? GL_TRUE : GL_FALSE);
}

GLAPI void APIENTRY
glGetShaderiv (GLuint shader_name, GLenum pname, GLint *params)
{
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const shader_t & shader = shader_t::storage().at(shader_name);

  if(pname == GL_SHADER_TYPE) {
    if(shader.type == RSXGL_VERTEX_SHADER) {
      *params = GL_VERTEX_SHADER;
    }
    else if(shader.type == RSXGL_FRAGMENT_SHADER) {
      *params = GL_FRAGMENT_SHADER;
    }
  }
  else if(pname == GL_DELETE_STATUS) {
    *params = shader.deleted;
  }
  else if(pname == GL_COMPILE_STATUS) {
    *params = shader.compiled;
  }
  else if(pname == GL_INFO_LOG_LENGTH) {
    *params = shader.info_size - 1;
  }
  else if(pname == GL_SHADER_SOURCE_LENGTH) {
    *params = shader.source_size - 1;
  }
  else {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetShaderInfoLog (GLuint shader_name, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const shader_t & shader = shader_t::storage().at(shader_name);

  shader.info().get(infoLog,bufSize);
  if(length != 0) *length = shader.info_size;

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetShaderSource (GLuint shader_name, GLsizei bufSize, GLsizei *length, GLchar *source)
{
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const shader_t & shader = shader_t::storage().at(shader_name);
  shader.source().get(source,bufSize);
  if(length != 0) *length = shader.info_size;

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glShaderBinary (GLsizei n, const GLuint* shader_names, GLenum binaryformat, const GLvoid* binary, GLsizei length)
{
  if(n < 0 || length < 0) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  for(GLsizei i = 0;i < n;++i,++shader_names) {
    if(!shader_t::storage().is_object(*shader_names)) {
      RSXGL_ERROR_(GL_INVALID_OPERATION);
    }

    shader_t & shader = shader_t::storage().at(*shader_names);

    if(binary != 0) {
      shader.binary().resize(length,0);
      shader.binary().set(binary,length);
    }
    else {
      shader.binary().resize(0);
    }

    shader.info().resize(0);
  }
  
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glShaderSource (GLuint shader_name, GLsizei count, const GLchar** string, const GLint* length)
{
  if(count < 0) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  // Concatenate the source:
  std::string source;

  if(string != 0) {
    for(GLsizei i = 0;i < count;++i,++string) {
      if(*string == 0) continue;
      
      if(length != 0) {
	source.append(*string,length[i]);
      }
      else {
	source.append(*string);
      }

      static std::string kNewline("\n");
      source.append(kNewline);
    }
  }

  shader_t & shader = shader_t::storage().at(shader_name);
  if(source.length() > 0) {
    const size_t n = source.length() + 1;
    shader.source().resize(n,0);
    shader.source().set(source.c_str(),n);
  }
  else {
    shader.source().resize(0);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glCompileShader (GLuint shader_name)
{
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  shader_t & shader = shader_t::storage().at(shader_name);
  shader.compiled = GL_FALSE;

  if(shader.source_data == 0) {
    RSXGL_NOERROR_();
  }

  compiler_context_t * cctx = current_ctx() -> compiler_context();
  rsxgl_assert(cctx != 0);

  cctx -> compile_shader(shader.mesa_shader,
			 shader.source_data);

  shader.compiled = shader.mesa_shader -> CompileStatus;
  const size_t n = strlen(shader.mesa_shader -> InfoLog) + 1;
  shader.info().resize(n);
  shader.info().set(shader.mesa_shader -> InfoLog,n);

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glReleaseShaderCompiler (void)
{
  // TODO: Return an error, because we don't really support this yet:
  //RSXGL_NOERROR_();
  RSXGL_ERROR_(GL_INVALID_OPERATION);
}

// Program functions:
program_t::program_t()
  : deleted(0), timestamp(0),
    linked(0), validated(0), invalid_uniforms(0), ref_count(0),
    attrib_name_max_length(0), uniform_name_max_length(0),
    mesa_program(0), nvfx_vp(0), nvfx_fp(0),
    vp_ucode_offset(~0), fp_ucode_offset(~0), vp_num_insn(0), fp_num_insn(0), 
    vp_input_mask(0), vp_output_mask(0), vp_num_internal_const(0),
    fp_control(0), instanceid_index(~0), point_sprite_control(0),
    uniform_values(0), program_offsets(0)
{
  attached_shaders().construct();
  linked_shaders().construct();

  info().construct();
  names().construct();

  attrib_table().construct();
  uniform_table().construct();
  sampler_uniform_table().construct();

  for(size_t i = 0;i < RSXGL_MAX_VERTEX_ATTRIBS;++i) {
    attrib_binding(i).construct();
  }

  for(size_t i = 0;i < RSXGL_MAX_DRAW_BUFFERS;++i) {
    fragout_binding(i).construct();
  }
}

program_t::~program_t()
{
  attached_shaders().destruct();
  for(unsigned int i = 0,n = attached_shaders().get_size();i < n;++i) {
    shader_t::gl_object_type::unref_and_maybe_delete(attached_shaders()[i]);
  }

  linked_shaders().destruct();
  for(unsigned int i = 0,n = linked_shaders().get_size();i < n;++i) {
    shader_t::gl_object_type::unref_and_maybe_delete(linked_shaders()[i]);
  }

  info().destruct();
  names().destruct();

  attrib_table().destruct();
  uniform_table().destruct();
  sampler_uniform_table().destruct();

  for(size_t i = 0;i < RSXGL_MAX_VERTEX_ATTRIBS;++i) {
    attrib_binding(i).destruct();
  }

  for(size_t i = 0;i < RSXGL_MAX_DRAW_BUFFERS;++i) {
    fragout_binding(i).destruct();
  }

  if(uniform_values != 0) free(uniform_values);
  if(program_offsets != 0) free(program_offsets);

  // TODO: delete microcode storage also
}

GLAPI GLuint APIENTRY
glCreateProgram (void)
{
  uint32_t name = program_t::storage().create_name_and_object();

  program_t & program = program_t::storage().at(name);
  compiler_context_t * cctx = current_ctx() -> compiler_context();
  program.mesa_program = cctx -> create_program();

  RSXGL_NOERROR(name);
}

GLAPI void APIENTRY
glDeleteProgram (GLuint program_name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  // TODO: orphan it, instead of doing this:
  program_t & program = program_t::storage().at(program_name);
  if(program.timestamp > 0) {
    rsxgl_timestamp_wait(current_ctx(),program.timestamp);
    program.timestamp = 0;
  }

  program_t::gl_object_type::maybe_delete(program_name);

  // TODO: destroy mesa resources
  
  RSXGL_NOERROR_();
}

GLAPI GLboolean APIENTRY
glIsProgram (GLuint program_name)
{
  RSXGL_NOERROR((program_t::storage().is_object(program_name) && program_t::storage().is_object(program_name)) ? GL_TRUE : GL_FALSE);
}

GLAPI void APIENTRY
glAttachShader (GLuint program_name, GLuint shader_name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  program_t & program = program_t::storage().at(program_name);
  shader_t & shader = shader_t::storage().at(shader_name);

  if(!program.attached_shaders().insert(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_OPERATION);
  }
  else {
    shader_t::gl_object_type::ref(shader_name);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glDetachShader (GLuint program_name, GLuint shader_name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }
  if(!shader_t::storage().is_object(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  program_t & program = program_t::storage().at(program_name);
  shader_t & shader = shader_t::storage().at(shader_name);

  if(!program.attached_shaders().erase(shader_name)) {
    RSXGL_ERROR_(GL_INVALID_OPERATION);
  }
  else {
    //compiler_context_t * cctx = current_ctx() -> compiler_context();
    //cctx -> dettach_shader(program.mesa_program,shader.mesa_shader);

    shader_t::gl_object_type::unref_and_maybe_delete(shader_name);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetAttachedShaders (GLuint program_name, GLsizei maxCount, GLsizei *count, GLuint *obj)
{
  if(maxCount < 0) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const program_t & program = program_t::storage().at(program_name);

  size_t m = 0;
  for(size_t i = 0,n = std::min((size_t)maxCount,(size_t)program.attached_shaders().get_size());i < n;++i) {
    *obj++ = program.attached_shaders()[i];
    ++m;
  }

  if(count != 0) {
    *count = m;
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetProgramiv (GLuint program_name, GLenum pname, GLint *params)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const program_t & program = program_t::storage().at(program_name);

  if(pname == GL_DELETE_STATUS) {
    *params = program.deleted;
  }
  else if(pname == GL_LINK_STATUS) {
    *params = program.linked;
  }
  else if(pname == GL_VALIDATE_STATUS) {
    *params = program.validated;
  }
  else if(pname == GL_INFO_LOG_LENGTH) {
    *params = program.info_size - 1;
  }
  else if(pname == GL_ATTACHED_SHADERS) {
#if 0
    size_t m = 0;
    for(size_t i = 0;i < RSXGL_MAX_SHADER_TYPES;++i) {
      if(program.attached_shaders.names[i] != 0) {
	++m;
      }
    }
    *params = m;
#endif
  }
  else if(pname == GL_ACTIVE_ATTRIBUTES) {
    if(program.linked) {
      *params = program.attrib_table_size;
    }
    else {
      *params = 0;
    }
  }
  else if(pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH) {
    if(program.linked) {
      *params = program.attrib_name_max_length;
    }
    else {
      *params = 0;
    }
  }
  else if(pname == GL_ACTIVE_UNIFORMS) {
    if(program.linked) {
      *params = program.uniform_table_size + program.sampler_uniform_table_size;
    }
    else {
      *params = 0;
    }
  }
  else if(pname == GL_ACTIVE_UNIFORM_MAX_LENGTH) {
    if(program.linked) {
      *params = program.uniform_name_max_length;
    }
    else {
      *params = 0;
    }
  }
  else if(pname == GL_TRANSFORM_FEEDBACK_BUFFER_MODE) {
  }
  else if(pname == GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH) {
  }
  else if(pname == GL_GEOMETRY_VERTICES_OUT) {
  }
  else if(pname == GL_GEOMETRY_INPUT_TYPE) {
  }
  else if(pname == GL_GEOMETRY_OUTPUT_TYPE) {
  }
  else {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetProgramInfoLog (GLuint program_name, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const program_t & program = program_t::storage().at(program_name);
  program.info().get(infoLog,bufSize);
  if(length != 0) *length = program.info_size;

  RSXGL_NOERROR_();
}

//
static void * main_ucode_address = 0;

static inline struct nvfx_vertex_program_exec *
rsxgl_main_ucode_address(program_t::ucode_offset_type off)
{
  rsxgl_assert(off != ~0U);
  return (struct nvfx_vertex_program_exec *)((uint8_t *)main_ucode_address + (ptrdiff_t)off * sizeof(struct nvfx_vertex_program_exec));
}

static inline program_t::ucode_offset_type
rsxgl_vp_ucode_offset(const struct nvfx_vertex_program_exec * address)
{
  return ((uint8_t *)address - (uint8_t *)main_ucode_address) / (sizeof(struct nvfx_vertex_program_exec));
}

static mspace
rsxgl_main_ucode_mspace()
{
  static const size_t size = 1024 * 1024;
  static mspace space = 0;

  if(space == 0) {
    main_ucode_address = memalign(RSXGL_CACHE_LINE_SIZE,size);
    rsxgl_assert(main_ucode_address != 0);

    space = create_mspace_with_base(main_ucode_address,size,0);
    rsxgl_assert(space != 0);
  }

  return space;
}

//
rsx_ptr_t rsx_ucode_address = 0;
uint32_t rsx_ucode_offset = 0;

static inline uint32_t *
rsxgl_rsx_ucode_address(program_t::ucode_offset_type off)
{
  rsxgl_assert(off != ~0U);
  return (uint32_t *)((uint8_t *)rsx_ucode_address + (ptrdiff_t)off * sizeof(uint32_t) * 4);
}

static inline uint32_t
rsxgl_rsx_ucode_offset(program_t::ucode_offset_type off)
{
  rsxgl_assert(off != ~0U);
  return (off * sizeof(uint32_t) * 4) + rsx_ucode_offset;
}

static inline program_t::ucode_offset_type
rsxgl_rsx_ucode_offset(const uint32_t * address)
{
  return ((uint8_t *)address - (uint8_t *)rsx_ucode_address) / (sizeof(uint32_t) * 4);
}

static mspace
rsxgl_rsx_ucode_mspace()
{
  static const size_t size = 4 * 1024 * 1024;
  static mspace space = 0;

  if(space == 0) {
    rsx_ucode_address = rsxgl_rsx_memalign(RSXGL_CACHE_LINE_SIZE,size);
    rsxgl_assert(rsx_ucode_address != 0);

    gcmAddressToOffset(rsx_ucode_address,&rsx_ucode_offset);

    space = create_mspace_with_base(rsx_ucode_address,size,0);
    rsxgl_assert(space != 0);
  }

  return space;
}

#if 0
template< typename Object, uint32_t Object::*PtrToMembersOffset,
	  typename Member, uint32_t Member::*PtrToNameOffset >
struct rsx_program_member_cmp {
  const uint8_t * blob;
  const Member * members;

  rsx_program_member_cmp(const uint8_t * _blob)
    : blob(_blob),
      members((const Member *)(blob + (*reinterpret_cast< const Object *>(blob)).*PtrToMembersOffset)) {
  }

  rsx_program_member_cmp(const Object * _object)
    : blob((const uint8_t *)_object),
      members((const Member *)(blob + (*_object.*PtrToMembersOffset))) {
  }

  const char * name(const Member * member) const {
    return (const char *)blob + *member.*PtrToNameOffset;
  }

  const char * name(program_t::attrib_size_type index) const {
    return name(members + index);
  }
};

template< typename Object, uint32_t Object::*PtrToMembersOffset,
	  typename Member, uint32_t Member::*PtrToNameOffset >
struct rsx_program_member_lt : public rsx_program_member_cmp< Object, PtrToMembersOffset, Member, PtrToNameOffset > {
  typedef rsx_program_member_cmp< Object, PtrToMembersOffset, Member, PtrToNameOffset > base_type;

  rsx_program_member_lt(const uint8_t * _blob)
    : base_type(_blob) {
  }

  rsx_program_member_lt(const Object * _object)
    : base_type(_object) {
  }

  bool operator()(const Member * lhs,const char * rhs) const {
    return strcmp(base_type::name(lhs),rhs) < 0;
  }

  bool operator()(const Member * lhs,const Member * rhs) const {
    return strcmp(base_type::name(lhs),base_type::name(rhs)) < 0;
  }
};

template< typename Object, uint32_t Object::*PtrToMembersOffset,
	  typename Member, uint32_t Member::*PtrToNameOffset >
struct rsx_program_member_eq : public rsx_program_member_cmp< Object, PtrToMembersOffset, Member, PtrToNameOffset > {
  typedef rsx_program_member_cmp< Object, PtrToMembersOffset, Member, PtrToNameOffset > base_type;

  rsx_program_member_eq(const uint8_t * _blob)
    : base_type(_blob) {
  }

  rsx_program_member_eq(const Object * _object)
    : base_type(_object) {
  }

  bool operator()(const Member * lhs,const char * rhs) const {
    return strcmp(base_type::name(lhs),rhs) == 0;
  }

  bool operator()(const Member * lhs,const Member * rhs) const {
    return strcmp(base_type::name(lhs),base_type::name(rhs)) == 0;
  }
};

template< typename InputIterator, typename T, typename LessThan, typename EqualTo >
InputIterator binary_find(InputIterator first,InputIterator last,
			  const T & value,const LessThan & lt,const EqualTo & eq)
{
  InputIterator it = std::lower_bound(first,last,value,lt);
  if(it != last && eq(*it,value)) {
    return it;
  }
  else {
    return last;
  }
}

struct uniform_lt {
  const char * names;

  uniform_lt(const char * _names)
    : names(_names) {
  }

  bool operator()(const program_t::uniform_table_type::value_type & lhs,const program_t::uniform_table_type::value_type & rhs) const {
    return strcmp(names + lhs.first,names + rhs.first) < 0;
  }
};
#endif

static inline uint8_t
rsxgl_glsl_type_to_rsxgl_type(const glsl_type * type)
{
  if(type -> base_type == GLSL_TYPE_FLOAT) {
    if(type -> vector_elements == 1) {
      return RSXGL_DATA_TYPE_FLOAT;
    }
    else if(type -> vector_elements == 2) {
      return RSXGL_DATA_TYPE_FLOAT2;
    }
    else if(type -> vector_elements == 3) {
      return RSXGL_DATA_TYPE_FLOAT3;
    }
    else if(type -> vector_elements == 4) {
      if(type -> matrix_columns == 4) {
	return RSXGL_DATA_TYPE_FLOAT4x4;
      }
      else {
	return RSXGL_DATA_TYPE_FLOAT4;
      }
    }
  }
  else if(type -> base_type == GLSL_TYPE_SAMPLER) {
    if(type -> sampler_dimensionality == GLSL_SAMPLER_DIM_1D) {
      return RSXGL_DATA_TYPE_SAMPLER1D;
    }
    else if(type -> sampler_dimensionality == GLSL_SAMPLER_DIM_2D) {
      return RSXGL_DATA_TYPE_SAMPLER2D;
    }
    else if(type -> sampler_dimensionality == GLSL_SAMPLER_DIM_3D) {
      return RSXGL_DATA_TYPE_SAMPLER3D;
    }
    else if(type -> sampler_dimensionality == GLSL_SAMPLER_DIM_CUBE) {
      return RSXGL_DATA_TYPE_SAMPLERCUBE;
    }
    else if(type -> sampler_dimensionality == GLSL_SAMPLER_DIM_RECT) {
      return RSXGL_DATA_TYPE_SAMPLERRECT;
    }
  }
  return RSXGL_DATA_TYPE_UNKNOWN;
}

GLAPI void APIENTRY
glLinkProgram (GLuint program_name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  program_t & program = program_t::storage().at(program_name);
  std::string info;

  compiler_context_t * cctx = current_ctx() -> compiler_context();

  for(unsigned int i = 0,n = program.attached_shaders().get_size();i < n;++i) {
    cctx -> attach_shader(program.mesa_program,shader_t::storage().at(program.attached_shaders()[i]).mesa_shader);
  }
  
  cctx -> link_program(program.mesa_program);

  program.nvfx_vp = cctx -> translate_vp(program.mesa_program);
  program.nvfx_fp = cctx -> translate_fp(program.mesa_program);

  rsxgl_debug_printf("%s result: %i info: %s programs: %lx %x\n",
		     __PRETTY_FUNCTION__,
		     program.mesa_program -> LinkStatus,
		     program.mesa_program -> InfoLog,
		     (unsigned long)program.nvfx_vp,
		     (unsigned long)program.nvfx_fp);

  info += std::string(program.mesa_program -> InfoLog);

  if(program.mesa_program -> LinkStatus) {
    // TODO: orphan it, instead of doing this:
    if(program.timestamp > 0) {
      rsxgl_timestamp_wait(current_ctx(),program.timestamp);
      program.timestamp = 0;
    }

    //
    program.linked = GL_TRUE;

    // Get rid of any linked shaders:
    for(unsigned int i = 0,n = program.linked_shaders().get_size();i < n;++i) {
      shader_t::gl_object_type::unref_and_maybe_delete(program.linked_shaders()[i]);
    }
    program.linked_shaders().destruct();

    // Move attached shaders to linked shaders:
    program.linked_shaders_data = program.attached_shaders_data;
    program.linked_shaders_size = program.attached_shaders_size;

    // Start a new attached shaders array:
    program.attached_shaders().construct();

    //
    // Migrate vertex program microcode to cache-aligned memory:
    {
      static const std::string kVPUcodeAllocFail("Failed to allocate space for vertex program microcode");
      
      struct nvfx_vertex_program_exec * address = (struct nvfx_vertex_program_exec *)mspace_memalign(rsxgl_main_ucode_mspace(),RSXGL_CACHE_LINE_SIZE,program.nvfx_vp -> nr_insns * sizeof(struct nvfx_vertex_program_exec));
      if(address == 0) {
	info += kVPUcodeAllocFail;
	//goto fail;
      }
      else {
	if(program.vp_ucode_offset != ~0U) {
	  mspace_free(rsxgl_main_ucode_mspace(),rsxgl_main_ucode_address(program.vp_ucode_offset));
	}
	
	program.vp_ucode_offset = rsxgl_vp_ucode_offset(address);
	
	memcpy(address,program.nvfx_vp -> insns,program.nvfx_vp -> nr_insns * sizeof(struct nvfx_vertex_program_exec));

	program.vp_num_insn = program.nvfx_vp -> nr_insns;
	program.vp_input_mask = program.nvfx_vp -> ir;
	program.vp_output_mask = program.nvfx_vp -> _or;
      }
    }
    
    // Migrate fragment program microcode to RSX memory:
    {
      static const std::string kFPUcodeAllocFail("Failed to allocate space for fragment program microcode");
      
      uint32_t * address = (uint32_t *)mspace_memalign(rsxgl_rsx_ucode_mspace(),RSXGL_CACHE_LINE_SIZE,program.nvfx_fp -> insn_len * sizeof(uint32_t));
      if(address == 0) {
	info += kFPUcodeAllocFail;
	//goto fail;
      }
      else {
	if(program.fp_ucode_offset != ~0U) {
	  mspace_free(rsxgl_rsx_ucode_mspace(),rsxgl_rsx_ucode_address(program.fp_ucode_offset));
	}
	
	program.fp_ucode_offset = rsxgl_rsx_ucode_offset(address);
	
	memcpy(address,program.nvfx_fp -> insn,program.nvfx_fp -> insn_len * sizeof(uint32_t));

	program.fp_num_insn = program.nvfx_fp -> insn_len / 4;
	program.fp_control = program.nvfx_fp -> fp_control;

#if 0	
	uint16_t
	  texcoords = fp -> texcoords,
	  texcoord2D = fp -> texcoord2D,
	  texcoord3D = fp -> texcoord3D;
	for(size_t i = 0;i < RSXGL_MAX_TEXTURE_COORDS;++i) {
	  if(texcoords & 1) program.fp_texcoords.set(i);
	  if(texcoord2D & 1) program.fp_texcoord2D.set(i);
	  if(texcoord3D & 1) program.fp_texcoord3D.set(i);
	  
	  texcoords >>= 1;
	  texcoord2D >>= 1;
	  texcoord3D >>= 1;
	}
#endif
      }
    }

    // Things that get accumulated:
    // program_offsets - uint32_t's
    // uniform_values - ieee32_t's
    // attribs - map from string's to attrib_t's
    // uniforms - map from string's to uniform_t's
    // sampler_uniforms - map from string's to sampler_uniform_t's
    // then iterate over attribs, uniforms, sampler uniforms, create names area

    std::deque< uint32_t > program_offsets;
    std::deque< ieee32_t > uniform_values;
    std::map< const char *, program_t::attrib_t > attribs;
    std::map< const char *, program_t::uniform_t > uniforms;
    std::map< const char *, program_t::sampler_uniform_t > sampler_uniforms;
    program_t::name_size_type names_size = 0;

    //
    struct gl_shader * gl_vsh = program.mesa_program->_LinkedShaders[MESA_SHADER_VERTEX];
    struct gl_program * gl_vp = gl_vsh->Program;
    
    struct gl_shader * gl_fsh = program.mesa_program->_LinkedShaders[MESA_SHADER_FRAGMENT];
    struct gl_program * gl_fp = gl_fsh->Program;
    
    // Process vertex program attributes:
    {
      rsxgl_debug_printf("attributes:\n");

      program.attrib_name_max_length = 0;
      
      struct st_vertex_program * st_vp = st_vertex_program((struct gl_vertex_program *)gl_vp);
      
      exec_list *ir = gl_vsh->ir;
      foreach_list(node, ir) {
	const ir_variable *const var = ((ir_instruction *) node)->as_variable();
	
	if (var == NULL
	    || var->mode != ir_var_in
	    || var->location == -1
	    || var->location < VERT_ATTRIB_GENERIC0)
	  continue;
	
	// vp -> input_to_index[var -> location] is the VP location:
	rsxgl_debug_printf("\t%s:%i -> %i\n",var->name,var->location,st_vp -> input_to_index[var -> location]);

	program_t::attrib_t attrib;
	attrib.type = rsxgl_glsl_type_to_rsxgl_type(var->type);
	attrib.index = st_vp -> input_to_index[var -> location];
	attribs.insert(std::make_pair(var -> name,attrib));

	const program_t::name_size_type name_length = strlen(var -> name);
	program.attrib_name_max_length = std::max(program.attrib_name_max_length,(program_t::name_size_type)name_length);
	names_size += name_length + 1;
      }
    }
    
    // Process program uniforms:
    {
      // Build vp constant map - from index into gl_vp -> Parameters to hardware index:
      // Also deal with vertex program immediates:
      typedef std::map< unsigned int, uint32_t > nvfx_vp_constant_map_t;
      nvfx_vp_constant_map_t nvfx_vp_constant_map;
      uint32_t vp_num_internal_const = 0;
      
      if(program.nvfx_vp != 0) {
	const struct nvfx_vertex_program_data * vp_const = program.nvfx_vp -> consts;
	for(unsigned int i = 0,n = program.nvfx_vp -> nr_consts;i < n;++i,++vp_const) {
	  if(vp_const -> index == -1) {
	    program_offsets.push_back(1);
	    program_offsets.push_back(i);

	    for(unsigned int j = 0;j < 4;++j) {
	      ieee32_t tmp;
	      tmp.f = vp_const -> value[j];
	      uniform_values.push_back(tmp);
	    }

	    ++vp_num_internal_const;
	  }
	  else {
	    nvfx_vp_constant_map[vp_const -> index] = i;
	  }
	}
      }

      program.vp_num_internal_const = vp_num_internal_const;
      
      // Build fp constant map - from index into gl_fp -> Parameters to a std::deque of offsets:
      typedef std::map< unsigned int, std::deque< uint32_t > > nvfx_fp_constant_map_t;
      nvfx_fp_constant_map_t nvfx_fp_constant_map;
      
      if(program.nvfx_fp != 0) {
	const struct nvfx_fragment_program_data * fp_const = program.nvfx_fp -> consts;
	for(unsigned int i = 0,n = program.nvfx_fp -> nr_consts;i < n;++i,++fp_const) {
	  nvfx_fp_constant_map[fp_const -> index].push_back(fp_const -> offset);
	}
      }
      
      rsxgl_debug_printf("%i uniforms:\n",program.mesa_program -> NumUserUniformStorage);
      
      for(unsigned int i = 0,n = program.mesa_program -> NumUserUniformStorage;i < n;++i) {
	const gl_uniform_storage * uniform_storage = program.mesa_program -> UniformStorage + i;
	const glsl_type * type = uniform_storage -> type;

	rsxgl_debug_printf("\t%s type:%s num_driver_storage:%u\n",
			   uniform_storage -> name,
			   uniform_storage -> type -> name,
			   uniform_storage -> num_driver_storage);

	if(uniform_storage -> type -> base_type != GLSL_TYPE_SAMPLER) {
	  program_t::uniform_t uniform;
	  uniform.type = rsxgl_glsl_type_to_rsxgl_type(type);
	  uniform.count = type -> matrix_columns;

	  uniform.values_index = uniform_values.size();
	  std::fill_n(std::back_inserter(uniform_values),type -> vector_elements * type -> matrix_columns,ieee32_t());
	  
	  // Search for it in vp:
	  bool found_vp = false, found_fp = false;
	  
	  for(unsigned int i = 0,n = gl_vp -> Parameters -> NumParameters;i < n;++i) {
	    gl_program_parameter * parameter = gl_vp -> Parameters -> Parameters + i;
	    if((parameter -> Type == PROGRAM_UNIFORM || parameter -> Type == PROGRAM_SAMPLER) && strcmp(parameter -> Name,uniform_storage -> name) == 0) {
	      if(!found_vp) rsxgl_debug_printf("\t\tfound in vp register type: %u data type: %x: ",parameter -> Type,parameter -> DataType);
	      found_vp |= true;
	      
	      if(parameter -> Type == PROGRAM_UNIFORM) {
		nvfx_vp_constant_map_t::const_iterator it = nvfx_vp_constant_map.find(i);
		if(it != nvfx_vp_constant_map.end()) {
		  rsxgl_debug_printf("%u ",it -> second);
		}
	      }
	      else if(parameter -> Type == PROGRAM_SAMPLER) {
		rsxgl_debug_printf("(sampler: %u)",(unsigned int)uniform_storage -> sampler);
	      }
	    }
	  }
	  if(found_vp) rsxgl_debug_printf("\n");
	  
	  // Search for it in fp:
	  // for each count:
	  // - store an offset count n
	  // - store n offsets

	  for(unsigned int i = 0,n = gl_fp -> Parameters -> NumParameters;i < n;++i) {
	    gl_program_parameter * parameter = gl_fp -> Parameters -> Parameters + i;
	    if((parameter -> Type == PROGRAM_UNIFORM || parameter -> Type == PROGRAM_SAMPLER) && strcmp(parameter -> Name,uniform_storage -> name) == 0) {
	      if(!found_fp) rsxgl_debug_printf("\t\tfound in fp register type: %u data type: %x: ",parameter -> Type,parameter -> DataType);
	      found_fp |= true;
	      
	      if(parameter -> Type == PROGRAM_UNIFORM) {
		nvfx_fp_constant_map_t::const_iterator it = nvfx_fp_constant_map.find(i);
		if(it != nvfx_fp_constant_map.end()) {
		  for(std::deque< uint32_t >::const_iterator jt = it -> second.begin();jt != it -> second.end();++jt) {
		    rsxgl_debug_printf("%u ",*jt);
		  }
		}
	      }
	      else if(parameter -> Type == PROGRAM_SAMPLER) {
		rsxgl_debug_printf("(sampler: %u)",(unsigned int)uniform_storage -> sampler);
	      }
	    }
	  }
	  if(found_fp) rsxgl_debug_printf("\n");

	  uniforms.insert(std::make_pair(uniform_storage -> name,uniform));
	}
	else {
	  program_t::sampler_uniform_t uniform;
	  uniform.type = rsxgl_glsl_type_to_rsxgl_type(uniform_storage -> type);

	  sampler_uniforms.insert(std::make_pair(uniform_storage -> name,uniform));
	}
      }
    }
  }
  
  if(info.length() > 0) {
    const size_t n = info.length() + 1;
    program.info().resize(n,0);
    program.info().set(info.c_str(),n);
  }
  else {
    program.info().resize(0);
  }

#if 0
  shader_t * shaders[RSXGL_MAX_SHADER_TYPES];
  size_t nValidShaders = 0;
  program.linked = GL_FALSE;

  static const std::string kMissingShader[RSXGL_MAX_SHADER_TYPES] = {
    "program does not have a vertex shader attached to it",
    "program does not have a fragment shader attached to it"
  };

  static const std::string kNoShaderBinary[RSXGL_MAX_SHADER_TYPES] = {
    "attached vertex shader does not have a binary",
    "attached fragment shader does not have a binary"
  };

  static const std::string kNewline("\n");

  rsxgl_debug_printf("%s\n",__PRETTY_FUNCTION__);

  // Some error checking - are there shaders attached, and do they have program binaries?
  for(size_t i = 0;i < RSXGL_MAX_SHADER_TYPES;++i) {
    shaders[i] = (program.attached_shaders.names[i] != 0) ? &program.attached_shaders[i] : 0;

    if(shaders[i] == 0) {
      info += kMissingShader[i] + kNewline;
    }
    // Vertex shader hasn't got a binary:
    else if(shaders[i] -> binary_size == 0) {
      info += kNoShaderBinary[i] + kNewline;
    }
    else {
      //program.linked_shaders.bind(i,program.attached_shaders.names[i]);
      ++nValidShaders;
    }
  }

  const rsxVertexProgram * vp = 0;
  const rsxFragmentProgram * fp = 0;

  //
  std::deque< const rsxProgramAttrib * > vp_attribs, vp_texture_attribs, vp_outputs;
  std::deque< const rsxProgramAttrib * > fp_inputs, fp_texture_attribs, fp_outputs;
  std::deque< const rsxProgramConst * > vp_consts, fp_consts;

  std::deque< std::pair< program_t::name_size_type, program_t::uniform_t > > uniforms;
  std::deque< std::pair< program_t::name_size_type, program_t::sampler_uniform_t > > textures;

  // VP internal constants:
  std::deque< const rsxProgramConst * > vp_internal_consts;

  //
  typedef std::deque< program_t::instruction_size_type > program_offsets_type;
  program_offsets_type program_offsets;

  // Number of uniform values:
  std::deque< ieee32_t > uniform_values;

  // Names that get accumulated:
  std::deque< std::pair< program_t::name_size_type, const char * > > names;
  program_t::name_size_type names_offset = 0;

  vp = reinterpret_cast< const rsxVertexProgram * >(shaders[0] -> binary_data);  
  fp = reinterpret_cast< const rsxFragmentProgram * >(shaders[1] -> binary_data);

  rsx_program_member_lt< rsxVertexProgram, &rsxVertexProgram::attrib_off, rsxProgramAttrib, &rsxProgramAttrib::name_off > vp_attrib_lt((const uint8_t *)vp);
  rsx_program_member_lt< rsxFragmentProgram, &rsxFragmentProgram::attrib_off, rsxProgramAttrib, &rsxProgramAttrib::name_off > fp_attrib_lt((const uint8_t *)fp);

  if(nValidShaders < RSXGL_MAX_SHADER_TYPES) {
    // Error messages were added previously.
    goto fail;
  }

  // TODO: orphan it, instead of doing this:
  if(program.timestamp > 0) {
    rsxgl_timestamp_wait(current_ctx(),program.timestamp);
    program.timestamp = 0;
  }

  // Migrate vertex program microcode to cache-aligned memory:
  {
    static const std::string kVPUcodeAllocFail("Failed to allocate space for vertex program microcode");

    program_t::vp_instruction_type * address = (program_t::vp_instruction_type *)mspace_memalign(rsxgl_main_ucode_mspace(),RSXGL_CACHE_LINE_SIZE,vp -> num_insn * sizeof(program_t::vp_instruction_type));
    if(address == 0) {
      info += kVPUcodeAllocFail;
      goto fail;
    }
    else {
      if(program.vp_ucode_offset != ~0U) {
	mspace_free(rsxgl_main_ucode_mspace(),rsxgl_main_ucode_address(program.vp_ucode_offset));
      }
      
      program.vp_ucode_offset = rsxgl_vp_ucode_offset(address);
      
      memcpy(address,(const uint8_t *)vp + vp -> ucode_off,vp -> num_insn * sizeof(program_t::vp_instruction_type));
      
      program.vp_num_insn = vp -> num_insn;
      program.vp_input_mask = vp -> input_mask;
      program.vp_output_mask = vp -> output_mask;
    }
  }
  
  // Migrate fragment program microcode to RSX memory:
  {
    static const std::string kFPUcodeAllocFail("Failed to allocate space for fragment program microcode");

    program_t::fp_instruction_type * address = (program_t::fp_instruction_type *)mspace_memalign(rsxgl_rsx_ucode_mspace(),RSXGL_CACHE_LINE_SIZE,fp -> num_insn * sizeof(program_t::fp_instruction_type));
    if(address == 0) {
      info += kFPUcodeAllocFail;
      goto fail;
    }
    else {
      if(program.fp_ucode_offset != ~0U) {
	mspace_free(rsxgl_rsx_ucode_mspace(),rsxgl_rsx_ucode_address(program.fp_ucode_offset));
      }
      
      program.fp_ucode_offset = rsxgl_rsx_ucode_offset(address);
      
      memcpy(address,(const uint8_t *)fp + fp -> ucode_off,fp -> num_insn * sizeof(program_t::fp_instruction_type));

      program.fp_num_insn = fp -> num_insn;
      program.fp_control = fp -> fp_control;
      program.fp_num_regs = fp -> num_regs;

      uint16_t
	texcoords = fp -> texcoords,
	texcoord2D = fp -> texcoord2D,
	texcoord3D = fp -> texcoord3D;
      for(size_t i = 0;i < RSXGL_MAX_TEXTURE_COORDS;++i) {
	if(texcoords & 1) program.fp_texcoords.set(i);
	if(texcoord2D & 1) program.fp_texcoord2D.set(i);
	if(texcoord3D & 1) program.fp_texcoord3D.set(i);

	texcoords >>= 1;
	texcoord2D >>= 1;
	texcoord3D >>= 1;
      }
    }
  }

  // Gather VP attributes:
  {
    const rsxProgramAttrib * _vp_attribs = rsxVertexProgramGetAttribs(const_cast< rsxVertexProgram * >(vp));

    for(program_t::attrib_size_type i = 0,n = vp -> num_attrib;i < n;++i,++_vp_attribs) {
      if(_vp_attribs -> index == -1) continue;

      const uint8_t type = _vp_attribs -> type;
      if(!(type == RSXGL_DATA_TYPE_SAMPLER1D ||
	   type == RSXGL_DATA_TYPE_SAMPLER2D ||
	   type == RSXGL_DATA_TYPE_SAMPLER3D ||
	   type == RSXGL_DATA_TYPE_SAMPLERCUBE ||
	   type == RSXGL_DATA_TYPE_SAMPLERRECT)) {
	if(!_vp_attribs -> is_output) {
	  vp_attribs.push_back(_vp_attribs);
	}
	else {
	  vp_outputs.push_back(_vp_attribs);
	}
      }
      else {
	vp_texture_attribs.push_back(_vp_attribs);
      }
    }
    
    std::sort(vp_attribs.begin(),vp_attribs.end(),vp_attrib_lt);
    std::sort(vp_texture_attribs.begin(),vp_texture_attribs.end(),vp_attrib_lt);
    std::sort(vp_outputs.begin(),vp_outputs.end(),vp_attrib_lt);
  }

  // Gather FP attributes:
  {
    const rsxProgramAttrib * _fp_attribs = rsxFragmentProgramGetAttribs(const_cast< rsxFragmentProgram * >(fp));

    for(program_t::attrib_size_type i = 0,n = fp -> num_attrib;i < n;++i,++_fp_attribs) {
      if(_fp_attribs -> index == -1) continue;

      const uint8_t type = _fp_attribs -> type;
      if(type == RSXGL_DATA_TYPE_SAMPLER1D ||
	 type == RSXGL_DATA_TYPE_SAMPLER2D ||
	 type == RSXGL_DATA_TYPE_SAMPLER3D ||
	 type == RSXGL_DATA_TYPE_SAMPLERCUBE ||
	 type == RSXGL_DATA_TYPE_SAMPLERRECT) {
	fp_texture_attribs.push_back(_fp_attribs);
      }
      else {
	if(!_fp_attribs -> is_output) {
	  fp_inputs.push_back(_fp_attribs);
	}
	else {
	  fp_outputs.push_back(_fp_attribs);
	}
      }
    }
    
    std::sort(fp_texture_attribs.begin(),fp_texture_attribs.end(),fp_attrib_lt);
    std::sort(fp_inputs.begin(),fp_inputs.end(),fp_attrib_lt);
    std::sort(fp_outputs.begin(),fp_outputs.end(),fp_attrib_lt);
  }

  {
    program.attrib_table().resize(vp_attribs.size());
    program.attrib_name_max_length = 0;
    for(program_t::attrib_size_type i = 0,n = vp_attribs.size();i < n;++i) {
      const rsxProgramAttrib * _attrib = vp_attribs[i];
      
      program.attrib_table_values[i].first = names_offset;
      program.attrib_table_values[i].second.type = _attrib -> type;
      program.attrib_table_values[i].second.index = _attrib -> index;
      
      const char * name = (const char *)((const uint8_t *)vp + _attrib -> name_off);
      const program_t::name_size_type name_length = strlen(name);
      
      program.attrib_name_max_length = std::max(program.attrib_name_max_length,name_length);
      names_offset += name_length + 1;
      names.push_back(std::make_pair(name_length,name));
    }

    // Iterate over attribute bindings; determine which attributes to swap:
    program_t::attrib_size_type attrib_binding[RSXGL_MAX_VERTEX_ATTRIBS];
    for(program_t::attrib_size_type src = 0;src < RSXGL_MAX_VERTEX_ATTRIBS;++src) {
      attrib_binding[src] = src;
    }
    
    rsx_program_member_eq< rsxVertexProgram, &rsxVertexProgram::attrib_off, rsxProgramAttrib, &rsxProgramAttrib::name_off > eq((const uint8_t *)vp);

    for(program_t::attrib_size_type dst = 0;dst < RSXGL_MAX_VERTEX_ATTRIBS;++dst) {
      program_t::attrib_binding_type::const_type binding(program.attrib_binding(dst));
      if(binding.size == 0) continue;

      std::deque< const rsxProgramAttrib * >::iterator it = binary_find(vp_attribs.begin(),vp_attribs.end(),binding.values,vp_attrib_lt,eq);
      if(it == vp_attribs.end()) continue;

      program_t::attrib_size_type src = (*it) -> index;
      program_t::attrib_size_type tmp = attrib_binding[src];

      attrib_binding[src] = dst;
      attrib_binding[dst] = tmp;
    }

    // Rewrite the attribute table. Sorted indices will be the re-bound "physical" vertex attribute indices:
    program.attribs_enabled.reset();

    for(program_t::attrib_size_type i = 0,n = program.attrib_table_size;i < n;++i) {
      const uint32_t old_index = program.attrib_table_values[i].second.index;
      const uint32_t new_index = attrib_binding[old_index];

      program.attrib_table_values[i].second.index = new_index;
      program.attribs_enabled.set(new_index);
    }

    // Patch VP microcode to reflect new binding:
    uint32_t vp_output_mask = 0;

    program_t::vp_instruction_type * vp_ucode = rsxgl_main_ucode_address(program.vp_ucode_offset);
    for(program_t::instruction_size_type i = 0,n = vp -> num_insn;i < n;++i,++vp_ucode) {
      uint32_t src_type = (vp_ucode -> words[2] >> NVFX_VP(INST_SRC0L_SHIFT)) & NVFX_VP(SRC_REG_TYPE_MASK);

      if(src_type == NVFX_VP(SRC_REG_TYPE_INPUT)) {
	const uint32_t src_index = (vp_ucode -> words[1] & NVFX_VP(INST_INPUT_SRC_MASK)) >> NVFX_VP(INST_INPUT_SRC_SHIFT);
	const uint32_t dst_index = attrib_binding[src_index];
	vp_ucode -> words[1] &= ~NVFX_VP(INST_INPUT_SRC_MASK);
	vp_ucode -> words[1] |= (dst_index << NVFX_VP(INST_INPUT_SRC_SHIFT)) & NVFX_VP(INST_INPUT_SRC_MASK);
      }
    }
  }

  // Uniform variable table:
  {
    // Sorted lists of vp and fp constants:    
    {
      const rsxProgramConst * _vp_const = rsxVertexProgramGetConsts(const_cast< rsxVertexProgram * >(vp));
      for(size_t i = 0,n = vp -> num_const;i < n;++i,++_vp_const) {
	if(_vp_const -> index == -1) continue;

	if(!_vp_const -> is_internal &&
	   (_vp_const -> type == RSXGL_DATA_TYPE_FLOAT ||
	    _vp_const -> type == RSXGL_DATA_TYPE_FLOAT2 ||
	    _vp_const -> type == RSXGL_DATA_TYPE_FLOAT3 ||
	    _vp_const -> type == RSXGL_DATA_TYPE_FLOAT4 ||
	    _vp_const -> type == RSXGL_DATA_TYPE_FLOAT4x4)) {
	  vp_consts.push_back(_vp_const);
	}
	// Vertex programs have "internal constants" which can't be changed by calls to glUniform
	// but nonetheless need to be submitted to the RSX every time a new program is activated.
	//
	// Their count and index are stuffed into the program_offsets list.
	// Their values are added to the beginning of the uniform_values list.
	// They are all assumed to be 4-element vectors - no effort made to save space.
	else if(_vp_const -> is_internal && _vp_const -> count > 0) {
	  vp_internal_consts.push_back(_vp_const);
	  program_offsets.push_back(_vp_const -> count);
	  program_offsets.push_back(_vp_const -> index);

	  std::copy(_vp_const -> values,_vp_const -> values + 4,std::back_inserter(uniform_values));
	  std::fill_n(std::back_inserter(uniform_values),4 * (_vp_const -> count - 1),ieee32_t());
	}
      }
      std::sort(vp_consts.begin(),vp_consts.end(),rsx_program_member_lt< rsxVertexProgram, &rsxVertexProgram::const_off, rsxProgramConst, &rsxProgramConst::name_off >(vp));
    }

    {
      const rsxProgramConst * _fp_const = rsxFragmentProgramGetConsts(const_cast< rsxFragmentProgram * >(fp));
      for(size_t i = 0,n = fp -> num_const;i < n;++i,++_fp_const) {
	if(_fp_const -> index == -1) continue;

	if(!_fp_const -> is_internal &&
	   (_fp_const -> type == RSXGL_DATA_TYPE_FLOAT ||
	    _fp_const -> type == RSXGL_DATA_TYPE_FLOAT2 ||
	    _fp_const -> type == RSXGL_DATA_TYPE_FLOAT3 ||
	    _fp_const -> type == RSXGL_DATA_TYPE_FLOAT4 ||
	    _fp_const -> type == RSXGL_DATA_TYPE_FLOAT4x4)) {
	  fp_consts.push_back(_fp_const);
	}
      }
      std::sort(fp_consts.begin(),fp_consts.end(),rsx_program_member_lt< rsxFragmentProgram, &rsxFragmentProgram::const_off, rsxProgramConst, &rsxProgramConst::name_off >(fp));
    }

    std::deque< const rsxProgramConst * >::const_iterator
      vp_it0 = vp_consts.begin(), vp_it1 = vp_consts.end(),
      fp_it0 = fp_consts.begin(), fp_it1 = fp_consts.end();

    program.uniform_name_max_length = 0;

    int mode = (vp_consts.size() > 0 && fp_consts.size() > 0) ? 0 : ((vp_consts.size() > 0) ? 1 : ((fp_consts.size() > 0) ? 2 : 3));
    while(mode < 3) {
      const char * name = 0;
      program_t::uniform_t uniform;
      const rsxProgramConst * the_const = 0;
      int c = 0;

      // Do this if comparing vp and fp constants:
      if(mode == 0) {
	const char * vp_name = reinterpret_cast< const char * >(vp) + (*vp_it0) -> name_off;
	const char * fp_name = reinterpret_cast< const char * >(fp) + (*fp_it0) -> name_off;
	
	c = strcmp(vp_name,fp_name);
	
	// If they are equal, then make sure they are of the same type & size:
	if(c == 0 && !((*vp_it0) -> type == (*fp_it0) -> type &&
		       (*vp_it0) -> count == (*fp_it0) -> count)) {
	  // TODO: Add an error message
	  goto fail;
	}
	
	name = (c <= 0) ? vp_name : fp_name;
	the_const = (c <= 0) ? *vp_it0 : *fp_it0;
	
	if(c <= 0) ++vp_it0;
	if(c >= 0) ++fp_it0;
      }
      // 
      else if(mode == 1) {
	name = reinterpret_cast< const char * >(vp) + (*vp_it0) -> name_off;
	the_const = *(vp_it0++);
      }
      // 
      else if(mode == 2) {
	name = reinterpret_cast< const char * >(fp) + (*fp_it0) -> name_off;
	the_const = *(fp_it0++);
      }

      // Space requirements for this uniform variable:
      size_t count = the_const -> count;
      
      // Skip if count == 0:
      if(count > 0) {
	uint8_t type = the_const -> type;
	size_t width = 0;
	switch(type) {
	case RSXGL_DATA_TYPE_FLOAT:
	  width = 1;
	  break;
	case RSXGL_DATA_TYPE_FLOAT2:
	  width = 2;
	  break;
	case RSXGL_DATA_TYPE_FLOAT3:
	  width = 3;
	  break;
	case RSXGL_DATA_TYPE_FLOAT4:
	  width = 4;
	  break;
	case RSXGL_DATA_TYPE_FLOAT4x4:
	  width = 4;
	  break;
	default:
	  width = 0;
	  break;
	};

	uniform.type = type;
	uniform.count = count;
	
	// Store the current index into the values array:
	uniform.values_index = uniform_values.size();
	
	// Increment by the number of ieee32_t's, not by number of bytes:
	std::copy(the_const -> values,the_const -> values + width,std::back_inserter(uniform_values));
	std::fill_n(std::back_inserter(uniform_values),width * (the_const -> count - 1),ieee32_t());
	
	// Set the vertex program index:
	if((mode == 0 && c <= 0) || (mode == 1)) {
	  uniform.enabled.set(RSXGL_VERTEX_SHADER);
	  uniform.vp_index = the_const -> index;
	}
	else {
	  uniform.vp_index = 0;
	}
	
	// Set the fragment program offset index, and accumulate more offsets:
	if((mode == 0 && c >= 0) || (mode == 2)) {
	  uniform.enabled.set(RSXGL_FRAGMENT_SHADER);
	  uniform.program_offsets_index = program_offsets.size();

	  for(size_t j = 0,m = the_const -> count;j < m;++j) {
	    rsxConstOffsetTable * table = rsxFragmentProgramGetConstOffsetTable(const_cast< rsxFragmentProgram * >(fp),the_const -> index + j);
	    
	    program_offsets.push_back(table -> num);
	    for(size_t k = 0,o = table -> num;k < o;++k) {
	      const uint32_t offset = table -> offset[k] / sizeof(program_t::fp_instruction_type);

	      program_offsets.push_back(offset);
	    }
	  }
	}
	else {
	  uniform.program_offsets_index = 0;
	}
	
	// Always do this:
	uniforms.push_back(std::make_pair(names_offset,uniform));
	
	program_t::name_size_type name_length = strlen(name);
	program.uniform_name_max_length = std::max(program.uniform_name_max_length,name_length);

	names_offset += name_length + 1;
	names.push_back(std::make_pair(name_length,name));
      }
	
      // 
      if(mode == 0) {
	if(vp_it0 == vp_it1 && fp_it0 == fp_it1) {
	  mode = 3;
	}
	else if(vp_it0 == vp_it1) {
	  mode = 2;
	}
	else if(fp_it0 == fp_it1) {
	  mode = 1;
	}
      }
      else if(mode == 1 && vp_it0 == vp_it1) {
	mode = 3;
      }
      else if(mode == 2 && fp_it0 == fp_it1) {
	mode = 3;
      }
    }

    // Fill in uniforms table:
    program.uniform_table().resize(uniforms.size());
    std::copy(uniforms.begin(),uniforms.end(),program.uniform_table_values);
  }

  // Texture table:
  {
    rsxgl_debug_printf("\ttexture table: %u vp textures, %u fp textures\n",vp_texture_attribs.size(),fp_texture_attribs.size());

    program.textures_enabled.reset();

    std::deque< const rsxProgramAttrib * >::const_iterator
      vp_it0 = vp_texture_attribs.begin(), vp_it1 = vp_texture_attribs.end(),
      fp_it0 = fp_texture_attribs.begin(), fp_it1 = fp_texture_attribs.end();
    
    int mode = (vp_texture_attribs.size() > 0 && fp_texture_attribs.size() > 0) ? 0 : ((vp_texture_attribs.size() > 0) ? 1 : ((fp_texture_attribs.size() > 0) ? 2 : 3));
    while(mode < 3) {
      const char * name = 0;
      program_t::sampler_uniform_t texture;
      const rsxProgramAttrib * the_attrib = 0;
      int c = 0;
      
      if(mode == 0) {
	const char * vp_name = reinterpret_cast< const char * >(vp) + (*vp_it0) -> name_off;
	const char * fp_name = reinterpret_cast< const char * >(fp) + (*fp_it0) -> name_off;
	
	c = strcmp(vp_name,fp_name);
	
	// If they are equal, then make sure they are of the same type & size:
	if(c == 0 && !((*vp_it0) -> type == (*fp_it0) -> type)) {
	  // TODO: Add an error message
	  goto fail;
	}
	
	name = (c <= 0) ? vp_name : fp_name;
	the_attrib = (c <= 0) ? *vp_it0 : *fp_it0;
	
	texture.vp_index = (*vp_it0) -> index;
	texture.fp_index = (*fp_it0) -> index;

	program.textures_enabled.set((*vp_it0) -> index);
	program.textures_enabled.set(RSXGL_MAX_VERTEX_TEXTURE_IMAGE_UNITS + (*fp_it0) -> index);
	
	if(c <= 0) ++vp_it0;
	if(c >= 0) ++fp_it0;
      }
      // 
      else if(mode == 1) {
	name = reinterpret_cast< const char * >(vp) + (*vp_it0) -> name_off;
	the_attrib = *(vp_it0++);

	texture.vp_index = the_attrib -> index;
	texture.fp_index = RSXGL_MAX_COMBINED_TEXTURE_IMAGE_UNITS;

	program.textures_enabled.set(the_attrib -> index);
      }
      // 
      else if(mode == 2) {
	name = reinterpret_cast< const char * >(fp) + (*fp_it0) -> name_off;
	the_attrib = *(fp_it0++);
	
	texture.vp_index = RSXGL_MAX_COMBINED_TEXTURE_IMAGE_UNITS;
	texture.fp_index = the_attrib -> index;

	program.textures_enabled.set(RSXGL_MAX_VERTEX_TEXTURE_IMAGE_UNITS + the_attrib -> index);
      }

      texture.type = the_attrib -> type;

      // Always do this:
      textures.push_back(std::make_pair(names_offset,texture));
      
      program_t::name_size_type name_length = strlen(name);
      program.uniform_name_max_length = std::max(program.uniform_name_max_length,name_length);
      
      names_offset += name_length + 1;
      names.push_back(std::make_pair(name_length,name));
      
      // 
      if(mode == 0) {
	if(vp_it0 == vp_it1 && fp_it0 == fp_it1) {
	  mode = 3;
	}
	else if(vp_it0 == vp_it1) {
	  mode = 2;
	}
	else if(fp_it0 == fp_it1) {
	  mode = 1;
	}
      }
      else if(mode == 1 && vp_it0 == vp_it1) {
	mode = 3;
      }
      else if(mode == 2 && fp_it0 == fp_it1) {
	mode = 3;
      }
    }

    // Fill in textures table:
    rsxgl_debug_printf("\ttotal of %u textures\n",textures.size());

    program.sampler_uniform_table().resize(textures.size());
    std::copy(textures.begin(),textures.end(),program.sampler_uniform_table_values);

    rsxgl_debug_printf("\tdone with texture table\n");
  }

  // Link VP outputs to FP inputs (by patching VP microcode) (GLSL varying variables):
  // TODO: Provide error information for unresolved varyings, or varyings whose types mismatch
  {
    // Map FP input indices to VP outputs:
    program_t::attrib_size_type linked_vp_index[RSXGL_MAX_VERTEX_ATTRIBS];
    for(program_t::attrib_size_type src = 0;src < RSXGL_MAX_VERTEX_ATTRIBS;++src) {
      //linked_vp_index[src] = src;
      linked_vp_index[src] = 0;
    }

    // Passing these following two types, which are declared in block scope, as template arguments is a C++0x feature, I believe.
    struct visitor {
      program_t::attrib_size_type * linked_vp_index;

      visitor(program_t::attrib_size_type * _vp_output_to_fp_input)
	: linked_vp_index(_vp_output_to_fp_input) {
      }

      void both(const rsxProgramAttrib * vp_output,const rsxProgramAttrib * fp_input) {
	rsxgl_assert(fp_input -> index < RSXGL_MAX_VERTEX_ATTRIBS);

	// FP's have 12 varying inputs (including input 0, window position); their corresponding VP output indices 
	// (the value included in the VP's output instructions) appear to be different. So this table establishes 
	// that correspondence:
	static const uint32_t fp_input_to_vp_output[12] = {
	  NV40_VP_INST_DEST_POS,
	  NV40_VP_INST_DEST_COL0,
	  NV40_VP_INST_DEST_COL1,
	  NV40_VP_INST_DEST_FOGC,
	  NV40_VP_INST_DEST_TC(0),
	  NV40_VP_INST_DEST_TC(1),
	  NV40_VP_INST_DEST_TC(2),
	  NV40_VP_INST_DEST_TC(3),
	  NV40_VP_INST_DEST_TC(4),
	  NV40_VP_INST_DEST_TC(5),
	  NV40_VP_INST_DEST_TC(6),
	  NV40_VP_INST_DEST_TC(7)
	};

	if(vp_output -> type == fp_input -> type) {
	  linked_vp_index[vp_output -> index] = fp_input_to_vp_output[fp_input -> index];
	}
	else {
	  // TODO: Generate an error message here, fail
	}
      }
    };

    struct compare {
      const rsxVertexProgram * vp;
      const rsxFragmentProgram * fp;

      compare(const rsxVertexProgram * _vp,const rsxFragmentProgram * _fp)
	: vp(_vp), fp(_fp) {
      }

      int operator()(const rsxProgramAttrib * vp_output,const rsxProgramAttrib * fp_input) const {
	return strcmp(reinterpret_cast< const char * >(vp) + vp_output -> name_off,reinterpret_cast< const char * >(fp) + fp_input -> name_off);
      }
    };

    set_intersection2(vp_outputs.begin(),vp_outputs.end(),fp_inputs.begin(),fp_inputs.end(),visitor(linked_vp_index),compare(vp,fp));

#if 0
    // Check to see if there are unresolved links between the VP and the FP:
    int unresolved_vp_outputs = 0;
    for(std::deque< const rsxProgramAttrib * >::const_iterator it = fp_inputs.begin(),it_end = fp_inputs.end();it != it_end;++it) {
      if(linked_vp_index[(*it) -> index] == -1) {
	// TODO: Generate an error:
	++unresolved_vp_outputs;
      }
    }

    if(unresolved_vp_outputs) {
      // TODO: Add an error message
      goto fail;
    }
#endif

    // Patch VP microcode to send VP outputs to correct FP input registers:
    uint32_t vp_output_mask = 0;

    program_t::vp_instruction_type * vp_ucode = rsxgl_main_ucode_address(program.vp_ucode_offset);
    for(program_t::instruction_size_type i = 0,n = vp -> num_insn;i < n;++i,++vp_ucode) {
      const uint32_t dword3 = vp_ucode -> words[3];
      uint32_t original_index = (dword3 & NV40_VP_INST_DEST_MASK);

      // Does this instruction write to some output register?
      if(original_index != NV40_VP_INST_DEST_MASK) {
	original_index >>= NV40_VP_INST_DEST_SHIFT;

	// Output to gl_Position, gl_ClipDistance, and gl_PointSize are ignored - we don't want to change those.
	if(original_index == 0) {
	  continue;
	}
	else if(original_index >= NV30_VP_INST_DEST_CLP(0) && original_index <= NV30_VP_INST_DEST_CLP(5)) {
	  vp_output_mask |= (1 << (original_index - NV30_VP_INST_DEST_CLP(0) + 6));
	}
	else if(original_index == NV40_VP_INST_DEST_PSZ) {
	  vp_output_mask |= (1 << 5);
	}
	//
	else {
	  const uint32_t linked_index = linked_vp_index[original_index];

	  // This instruction outputs to a register that isn't consumed by the fragment program. So
	  // replace the instruction with a NOP.
	  if(linked_index == 0) {
	    vp_ucode -> words[0] = 0;
	    vp_ucode -> words[1] = 0;
	    vp_ucode -> words[2] = 0;
	    vp_ucode -> words[3] = 0;
	  }
	  else {
	    vp_ucode -> words[3] = (dword3 & ~NV40_VP_INST_DEST_MASK) | ((linked_index << NV40_VP_INST_DEST_SHIFT) & NV40_VP_INST_DEST_MASK);

	    if(linked_index == NV40_VP_INST_DEST_COL0) {
	      vp_output_mask |= (1 << 0);
	    }
	    else if(linked_index == NV40_VP_INST_DEST_COL1) {
	      vp_output_mask |= (1 << 1);
	    }
	    else if(linked_index == NV40_VP_INST_DEST_BFC0) {
	      vp_output_mask |= (1 << 2);
	    }
	    else if(linked_index == NV40_VP_INST_DEST_BFC1) {
	      vp_output_mask |= (1 << 3);
	    }
	    else if(linked_index == NV40_VP_INST_DEST_FOGC) {
	      vp_output_mask |= (1 << 4);
	    }
	    else if(linked_index >= NV40_VP_INST_DEST_TC(0) && linked_index <= NV40_VP_INST_DEST_TC(7)) {
	      vp_output_mask |= (1 << (linked_index - NV40_VP_INST_DEST_TC0 + 14));
	    }
	  }
	}
      }
    }

    program.vp_output_mask = vp_output_mask;
  }

  // Assemble names:
  program.names().resize(names_offset);
  names_offset = 0;
  for(std::deque< std::pair< program_t::name_size_type, const char * > >::const_iterator it0 = names.begin(),it1 = names.end();it0 != it1;++it0) {
    memcpy(program.names_data + names_offset,it0 -> second,it0 -> first + 1);
    names_offset += it0 -> first + 1;
  }

  // Allocate space for uniform values:
  if(program.uniform_values != 0) free(program.uniform_values);
  program.uniform_values = (ieee32_t *)memalign(RSXGL_CACHE_LINE_SIZE,sizeof(ieee32_t) * uniform_values.size());
  std::copy(uniform_values.begin(),uniform_values.end(),program.uniform_values);

  // Deal with vp internal constants:
  program.vp_num_internal_const = vp_internal_consts.size();

  // Assemble program offsets:
  if(program.program_offsets != 0) free(program.program_offsets);
  program.program_offsets = (program_t::instruction_size_type *)memalign(RSXGL_CACHE_LINE_SIZE,sizeof(program_t::instruction_size_type) * program_offsets.size());
  std::copy(program_offsets.begin(),program_offsets.end(),program.program_offsets);  

  program.linked = GL_TRUE;

  // Resolve rsxgl_InstanceID in vertex program:
  {
    std::deque< const rsxProgramConst * >::iterator it = binary_find(vp_consts.begin(),vp_consts.end(),"rsxgl_InstanceID",
								     rsx_program_member_lt< rsxVertexProgram, &rsxVertexProgram::const_off, rsxProgramConst, &rsxProgramConst::name_off >(vp),
								     rsx_program_member_eq< rsxVertexProgram, &rsxVertexProgram::const_off, rsxProgramConst, &rsxProgramConst::name_off >(vp));
    if(it != vp_consts.end()) {
      rsxgl_debug_printf("rsxgl_InstanceID index: %u\n",(uint32_t)(*it) -> index);

      program.instanceid_index = (*it) -> index;
    }
    else {
      program.instanceid_index = ~0;
    }
  }

  // Resolve gl_PointCoord in fragment program:
  {
    rsx_program_member_eq< rsxFragmentProgram, &rsxFragmentProgram::attrib_off, rsxProgramAttrib, &rsxProgramAttrib::name_off > eq((const uint8_t *)fp);
    std::deque< const rsxProgramAttrib * >::iterator it = binary_find(fp_inputs.begin(),fp_inputs.end(),"gl_PointCoord",fp_attrib_lt,eq);
    if(it != fp_inputs.end()) {
      const uint32_t index = ((*it) -> index) - 4;
      rsxgl_assert(index < 8);

      program.point_sprite_control = (1 << (8 + index)) | 1;
      program.vp_output_mask |= (1 << (index + 14));
    }
    else {
      program.point_sprite_control = 0;
    }
  }

  // Move attached shaders to linked shaders:
  for(size_t i = 0;i < RSXGL_MAX_SHADER_TYPES;++i) {
    program.linked_shaders.bind(i,program.attached_shaders.names[i]);
  }

  // Unattach:
  for(size_t i = 0;i < RSXGL_MAX_SHADER_TYPES;++i) {
    program.attached_shaders.bind(i,0);
  }

  rsxgl_debug_printf("%s finished normally\n",__PRETTY_FUNCTION__);

 fail:

  rsxgl_debug_printf("%s finishing\n",__PRETTY_FUNCTION__);

  if(info.length() > 0) {
    const size_t n = info.length() + 1;
    program.info().resize(n,0);
    program.info().set(info.c_str(),n);
  }
  else {
    program.info().resize(0);
  }
#endif

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glValidateProgram (GLuint program_name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  program_t & program = program_t::storage().at(program_name);

  if(program.linked) {
    program.validated = GL_TRUE;
  }
  else {
    program.validated = GL_FALSE;
  }
  
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glUseProgram (GLuint program_name)
{
  struct rsxgl_context_t * ctx = current_ctx();

  if(program_name != 0 && !program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(ctx -> program_binding.names[RSXGL_ACTIVE_PROGRAM] != program_name) {
    const program_t::name_type prev_program_name = ctx -> program_binding.names[RSXGL_ACTIVE_PROGRAM];

    ctx -> program_binding.bind(RSXGL_ACTIVE_PROGRAM,program_name);
    ctx -> invalid.parts.program = 1;

    if(prev_program_name != 0) {
      const program_t::texture_assignments_bitfield_type
	textures_enabled = program_t::storage().at(prev_program_name).textures_enabled;
      const program_t::texture_assignments_type
	prev_assignments = program_t::storage().at(prev_program_name).texture_assignments,
	assignments = program_t::storage().at(program_name).texture_assignments;

      program_t::texture_assignments_bitfield_type::const_iterator
	enabled_it = textures_enabled.begin();
      program_t::texture_assignments_type::const_iterator
	prev_it = prev_assignments.begin(),
	it = assignments.begin();

      for(program_t::texture_size_type i = 0;i < program_t::texture_assignments_bitfield_type::size;++i,enabled_it.next(textures_enabled),prev_it.next(prev_assignments),it.next(assignments)) {
	ctx -> invalid_texture_assignments.set(enabled_it.test() && (prev_it.value() != it.value()));
      }
    }
    else {
      ctx -> invalid_texture_assignments = ctx -> program_binding[RSXGL_ACTIVE_PROGRAM].textures_enabled;
    }
  }

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glBindAttribLocation (GLuint program_name, GLuint index, const GLchar* name)
{
  if(index >= RSXGL_MAX_VERTEX_ATTRIBS) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  program_t & program = program_t::storage().at(program_name);
  
  const size_t n = strlen(name) + 1;

  program.attrib_binding(index).resize(n);
  program.attrib_binding(index).set(name,n);
  
  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetActiveAttrib (GLuint program_name, GLuint index, GLsizei bufsize, GLsizei* length, GLint* size, GLenum* type, GLchar* name)
{
  if(bufsize < 0) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const program_t & program = program_t::storage().at(program_name);

  if(!program.linked) {
    if(length != 0) *length = 0;
    if(bufsize > 0) *name = 0;
    RSXGL_NOERROR_();
  }

  if(index >= program.attrib_table_size) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const char * attrib_name = program.names_data + program.attrib_table_values[index].first;

  size_t n = std::min((size_t)bufsize - 1,(size_t)strlen(attrib_name));
  strncpy(name,attrib_name,n);
  name[n] = 0;

  if(length != 0) *length = n;

  switch(program.attrib_table_values[index].second.type) {
  case RSXGL_DATA_TYPE_FLOAT:
    *type = GL_FLOAT;
    *size = 1;
    break;
  case RSXGL_DATA_TYPE_FLOAT2:
    *type = GL_FLOAT_VEC2;
    *size = 1;
    break;
  case RSXGL_DATA_TYPE_FLOAT3:
    *type = GL_FLOAT_VEC3;
    *size = 1;
    break;
  case RSXGL_DATA_TYPE_FLOAT4:
    *type = GL_FLOAT_VEC4;
    *size = 1;
    break;
  case RSXGL_DATA_TYPE_FLOAT4x4:
    *type = GL_FLOAT_MAT4;
    *size = 1;
    break;
  default:
    *type = GL_NONE;
    *size = 0;
  };

  RSXGL_NOERROR_();
}

GLAPI GLint APIENTRY
glGetAttribLocation (GLuint program_name, const GLchar* name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR(GL_INVALID_VALUE,-1);
  }

  const program_t & program = program_t::storage().at(program_name);

  if(!program.linked) {
    RSXGL_NOERROR(-1);
  }

  std::pair< bool, program_t::attrib_size_type > tmp = program.attrib_table().find(program.names(),name);
  
  RSXGL_NOERROR((tmp.first) ? program.attrib_table_values[tmp.second].second.index : -1);
}

GLAPI void APIENTRY
glGetActiveUniform (GLuint program_name, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
{
  if(bufSize < 0) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const program_t & program = program_t::storage().at(program_name);

  if(!program.linked) {
    if(length != 0) *length = 0;
    if(bufSize > 0) *name = 0;
    RSXGL_NOERROR_();
  }

  if(index >= (program.uniform_table_size + program.sampler_uniform_table_size)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  const char * uniform_name = "";
  uint8_t uniform_type = ~0;

  if(index < program.uniform_table_size) {
    uniform_name = program.names_data + program.uniform_table_values[index].first;
    uniform_type = program.uniform_table_values[index].second.type;
  }
  else {
    const GLuint texture_index = index - program.uniform_table_size;

    uniform_name = program.names_data + program.sampler_uniform_table_values[texture_index].first;
    uniform_type = program.sampler_uniform_table_values[texture_index].second.type;
  }

  size_t n = std::min((size_t)bufSize - 1,(size_t)strlen(uniform_name));
  strncpy(name,uniform_name,n);
  name[n] = 0;

  if(length != 0) *length = n;

  switch(uniform_type) {
  case RSXGL_DATA_TYPE_FLOAT:
    *type = GL_FLOAT;
    rsxgl_assert(index < program.uniform_table_size);
    *size = program.uniform_table_values[index].second.count;
    break;
  case RSXGL_DATA_TYPE_FLOAT2:
    *type = GL_FLOAT_VEC2;
    rsxgl_assert(index < program.uniform_table_size);
    *size = program.uniform_table_values[index].second.count;
    break;
  case RSXGL_DATA_TYPE_FLOAT3:
    *type = GL_FLOAT_VEC3;
    rsxgl_assert(index < program.uniform_table_size);
    *size = program.uniform_table_values[index].second.count;
    break;
  case RSXGL_DATA_TYPE_FLOAT4:
    *type = GL_FLOAT_VEC4;
    rsxgl_assert(index < program.uniform_table_size);
    *size = program.uniform_table_values[index].second.count;
    break;
  case RSXGL_DATA_TYPE_FLOAT4x4:
    *type = GL_FLOAT_MAT4;
    rsxgl_assert(index < program.uniform_table_size);
    *size = program.uniform_table_values[index].second.count;
    break;
  case RSXGL_DATA_TYPE_SAMPLER1D:
    *type = GL_SAMPLER_1D;
    *size = 1;
  case RSXGL_DATA_TYPE_SAMPLER2D:
    *type = GL_SAMPLER_2D;
    *size = 1;
  case RSXGL_DATA_TYPE_SAMPLER3D:
    *type = GL_SAMPLER_3D;
    *size = 1;
  case RSXGL_DATA_TYPE_SAMPLERCUBE:
    *type = GL_SAMPLER_CUBE;
    *size = 1;
  case RSXGL_DATA_TYPE_SAMPLERRECT:
    *type = GL_SAMPLER_2D_RECT;
    *size = 1;
  default:
    *type = GL_NONE;
    *size = 0;
  };

  RSXGL_NOERROR_();
}

GLAPI int APIENTRY
glGetUniformLocation (GLuint program_name, const GLchar* name)
{
  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR(GL_INVALID_VALUE,-1);
  }

  const program_t & program = program_t::storage().at(program_name);

  if(!program.linked) {
    RSXGL_NOERROR(-1);
  }

  const std::pair< bool, program_t::uniform_size_type > tmp = program.uniform_table().find(program.names(),name);
  if(tmp.first) {
    RSXGL_NOERROR(tmp.second);
  }
  else {
    const std::pair< bool, program_t::texture_size_type > tmp = program.sampler_uniform_table().find(program.names(),name);
    RSXGL_NOERROR(tmp.first ? program.uniform_table_size + tmp.second : -1);
  }
}

GLAPI void APIENTRY
glBindFragDataLocation (GLuint program_name, GLuint color, const GLchar *name)
{
  if(color >= RSXGL_MAX_DRAW_BUFFERS) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(!program_t::storage().is_object(program_name)) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  program_t & program = program_t::storage().at(program_name);
  
  const size_t n = strlen(name) + 1;
  program.fragout_binding(color).resize(n);
  program.fragout_binding(color).set(name,n);
  
  RSXGL_NOERROR_();
}

GLAPI GLint APIENTRY
glGetFragDataLocation (GLuint program, const GLchar *name)
{
}

void
rsxgl_program_validate(rsxgl_context_t * ctx,const uint32_t timestamp)
{
  gcmContextData * context = ctx -> base.gcm_context;

  if(ctx -> program_binding.names[RSXGL_ACTIVE_PROGRAM] != 0) {
    program_t & program = ctx -> program_binding[RSXGL_ACTIVE_PROGRAM];

    rsxgl_assert(timestamp >= program.timestamp);
    program.timestamp = timestamp;    
  }

  if(ctx -> invalid.parts.program == 0) return;

  if(ctx -> program_binding.names[RSXGL_ACTIVE_PROGRAM] != 0) {
    program_t & program = ctx -> program_binding[RSXGL_ACTIVE_PROGRAM];

    // load the vertex program:
    {
      uint32_t * buffer = gcm_reserve(context,program.vp_num_insn * 5 + 7);
      
      gcm_emit_method(&buffer,NV30_3D_VP_UPLOAD_FROM_ID,1);
      gcm_emit(&buffer,0);

      const struct nvfx_vertex_program_exec * ucode = rsxgl_main_ucode_address(program.vp_ucode_offset);
      for(size_t i = 0,n = program.vp_num_insn;i < n;++i,++ucode) {
	gcm_emit_method(&buffer,NV30_3D_VP_UPLOAD_INST(0),4);
	gcm_emit(&buffer,ucode -> data[0]);
	gcm_emit(&buffer,ucode -> data[1]);
	gcm_emit(&buffer,ucode -> data[2]);
	gcm_emit(&buffer,ucode -> data[3]);
      }
      
      gcm_emit_method(&buffer,NV30_3D_VP_START_FROM_ID,1);
      gcm_emit(&buffer,0);
      gcm_emit_method(&buffer,NV40_3D_VP_ATTRIB_EN,2);
      gcm_emit(&buffer,program.vp_input_mask);
      gcm_emit(&buffer,program.vp_output_mask);
      
      gcm_finish_commands(context,&buffer);
    }

    // load vertex program internal constants:
    if(program.vp_num_internal_const > 0) {
      const program_t::instruction_size_type * program_offsets = program.program_offsets;
      const ieee32_t * uniform_values = program.uniform_values;
      for(program_t::uniform_size_type i = 0,n = program.vp_num_internal_const;i < n;++i) {
	program_t::instruction_size_type count = *program_offsets++;
	program_t::instruction_size_type index = *program_offsets++;

	uint32_t * buffer = gcm_reserve(context,6 * count);

	for(;count > 0;--count,++index,uniform_values += 4) {
	  gcm_emit_method(&buffer,NV30_3D_VP_UPLOAD_CONST_ID,5);
	  gcm_emit(&buffer,index);
	  
	  gcm_emit(&buffer,uniform_values[0].u);
	  gcm_emit(&buffer,uniform_values[1].u);
	  gcm_emit(&buffer,uniform_values[2].u);
	  gcm_emit(&buffer,uniform_values[3].u);
	}

	gcm_finish_commands(context,&buffer);
      }
    }
    
    // load the fragment program:
    {
      uint32_t i = 0;
      const uint32_t n = 4 + (2 * RSXGL_MAX_TEXTURE_COORDS);

      uint32_t * buffer = gcm_reserve(context,n);
      
      gcm_emit_method_at(buffer,i++,NV30_3D_FP_ACTIVE_PROGRAM,1);
      gcm_emit_at(buffer,i++,rsxgl_rsx_ucode_offset(program.fp_ucode_offset) | NV30_3D_FP_ACTIVE_PROGRAM_DMA0);

      // Texcoord control:
#define  NV40TCL_TEX_COORD_CONTROL(x)                                   (0x00000b40+((x)*4))
      bit_set< RSXGL_MAX_TEXTURE_COORDS >::const_iterator it = program.fp_texcoords.begin(),
	it2D = program.fp_texcoord2D.begin(),
	it3D = program.fp_texcoord3D.begin();
      uint32_t reg = 0xb40;
      for(uint32_t j = 0;j < RSXGL_MAX_TEXTURE_COORDS;++j,i += 2) {
#if 0
	const uint32_t cmds[2] = {
	  //NV40TCL_TEX_COORD_CONTROL(j),
	  reg,
	  it.test() ? 
	  ((it3D.test() ? (1 << 4) : 0) | (it2D.test() ? 1 : 0)) :
	  (0)
	};
	
	rsxgl_debug_printf("%u %u, %u %u %u: %x %x\n",
			   j,i,
			   (uint32_t)it.test(),
			   (uint32_t)it2D.test(),
			   (uint32_t)it3D.test(),
			   cmds[0],cmds[1]);

	gcm_emit_method_at(buffer,i,cmds[0],1);
	gcm_emit_at(buffer,i + 1,cmds[1]);
#endif

	gcm_emit_method_at(buffer,i,reg,1);
	gcm_emit_at(buffer,i + 1,
		    it.test() ? 
		    ((it3D.test() ? (1 << 4) : 0) | (it2D.test() ? 1 : 0)) :
		    (0));
	
	it.next(program.fp_texcoords);
	it2D.next(program.fp_texcoord2D);
	it3D.next(program.fp_texcoord3D);
	reg += 4;
      }

      gcm_emit_method_at(buffer,i++,NV30_3D_FP_CONTROL,1);
      gcm_emit_at(buffer,i++,program.fp_control);

      gcm_finish_n_commands(context,n);
    }

    // invalidate vertex program uniforms:
    if(program.uniform_table_size > 0) {
      program.invalid_uniforms = 1;
      program_t::uniform_table_type::value_type * uniform = program.uniform_table_values;
      for(program_t::uniform_size_type i = 0,n = program.uniform_table_size;i < n;++i,++uniform) {
	if(uniform -> second.enabled.test(RSXGL_VERTEX_SHADER)) {
	  uniform -> second.invalid.set(RSXGL_VERTEX_SHADER);
	}
      }
    }

    // Tell the GPU which vertex attributes the program will use:
    {
      uint32_t * buffer = gcm_reserve(context,2);
      
      gcm_emit_method(&buffer,NV40_3D_VP_ATTRIB_EN,1);
      gcm_emit(&buffer,program.attribs_enabled.as_integer());
      
      gcm_finish_commands(context,&buffer);
    }

    // Tell unused attributes to have a size of 0:
    {
      const bit_set< RSXGL_MAX_VERTEX_ATTRIBS > unused_attribs = ~program.attribs_enabled;

      if(unused_attribs.any()) {
	uint32_t * buffer = gcm_reserve(context,2 * RSXGL_MAX_VERTEX_ATTRIBS);
	size_t nbuffer = 0;

	for(size_t i = 0;i < RSXGL_MAX_VERTEX_ATTRIBS;++i) {
	  if(!unused_attribs.test(i)) continue;

	  gcm_emit_method_at(buffer,nbuffer + 0,NV30_3D_VTXFMT(i),1);
	  gcm_emit_at(buffer,nbuffer + 1,
		      /* ((uint32_t)attribs.frequency[i] << 16 | */
		      ((uint32_t)0 << NV30_3D_VTXFMT_STRIDE__SHIFT) |
		      ((uint32_t)0 << NV30_3D_VTXFMT_SIZE__SHIFT) |
		      ((uint32_t)RSXGL_VERTEX_F32 & 0x7));

	  nbuffer += 2;
	}

	gcm_finish_n_commands(context,nbuffer);
      }
    }

    // Set point sprite behavior:
    {
      if(program.point_sprite_control == 0) {
	uint32_t * buffer = gcm_reserve(context,2);

	gcm_emit_method_at(buffer,0,NV30_3D_POINT_PARAMETERS_ENABLE,1);
	gcm_emit_at(buffer,1,0);

	gcm_finish_n_commands(context,2);
      }
      else {
	//rsxgl_debug_printf("point sprite control: %x\n",program.point_sprite_control);

	uint32_t * buffer = gcm_reserve(context,4);

	gcm_emit_method_at(buffer,0,NV30_3D_POINT_PARAMETERS_ENABLE,1);
	gcm_emit_at(buffer,1,1);

	gcm_emit_method_at(buffer,2,NV30_3D_POINT_SPRITE,1);
	gcm_emit_at(buffer,3,program.point_sprite_control);

	gcm_finish_n_commands(context,4);
      }
    }
  }
}
