
#include <vectormath/cpp/vectormath_aos.h>

#include <EGL/egl.h>
#define GL3_PROTOTYPES
//#define _GNU_SOURCE
#include <GL3/gl3.h>
#include <GL3/gl3ext.h>
#include <GL3/rsxgl.h>
#include <GL3/rsxgl3ext.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sysmodule/sysmodule.h>

#include <vectormath/cpp/vectormath_aos.h>
#include "rsxgl_config.h"
#include "nagel_bin.h"
#include "gradient_png.h"

#include <png.h>
#include <pngdec/pngdec.h>
#include <rsx/commands.h>

#include "texcube_vert.h"
#include "texcube_frag.h"


const float geometry[] = {
   0,0,0, 1,0,
   1,0,0, 1,1,
   1,1,0, 0,1,
   0,1,0, 0,0,
   1,0,1, 1,0,
   1,1,1, 0,0,
   0,1,1, 0,1,
   0,0,1, 1,1,
   0,1,1, 1,0,
   0,1,0, 1,1,
   1,0,1, 0,1,
   1,0,0, 0,0
};


GLuint* client_indices = 0;
f32 rotx = 0.0f;
f32 roty = 0.0f;

const GLuint indices[] = {

      0,1,2,   0,2,3,   1,4,5,   1,5,2,   4,7,6,	 4,6,5,
                            7,0,3,   7,3,6,   9,2,5,   9,5,8,   0,10,11,   0,7,10
};

// Test program might want to use these:
int rsxgltest_width = 0, rsxgltest_height = 0;

static void report_glerror(const char * label)
{
  GLenum e = glGetError();
  if(e != GL_NO_ERROR) {
    if(label != 0) {
      printf("%s: %x\n",label,e);
    }
    else {
      printf("%x\n",e);
    }
  }
}

static void
report_shader_info(GLuint shader)
{
  GLint type = 0, delete_status = 0, compile_status = 0;

  if(glIsShader(shader)) {
    glGetShaderiv(shader,GL_SHADER_TYPE,&type);
    glGetShaderiv(shader,GL_DELETE_STATUS,&delete_status);
    glGetShaderiv(shader,GL_COMPILE_STATUS,&compile_status);
    
    printf("shader: %u type: %x compile_status: %i delete_status: %i\n",shader,type,compile_status,delete_status);

    GLint nInfo = 0;
    glGetShaderiv(shader,GL_INFO_LOG_LENGTH,&nInfo);
    if(nInfo > 0) {
      printf("\tinfo length: %u\n",nInfo);
      char szInfo[nInfo + 1];
      glGetShaderInfoLog(shader,nInfo + 1,0,szInfo);
      printf("\tinfo: %s\n",szInfo);
    }

  }
  else {
    printf("%u is not a shader\n",shader);
  }
}

static void
report_program_info(GLuint program)
{
  if(glIsProgram(program)) {
    GLint delete_status = 0, link_status = 0, validate_status = 0;

    glGetProgramiv(program,GL_DELETE_STATUS,&delete_status);
    glGetProgramiv(program,GL_LINK_STATUS,&link_status);
    glGetProgramiv(program,GL_VALIDATE_STATUS,&validate_status);
    
    printf("program: %u link_status: %i validate_status: %i delete_status: %i\n",program,link_status,validate_status,delete_status);

    GLint num_attached = 0;
    glGetProgramiv(program,GL_ATTACHED_SHADERS,&num_attached);
    printf("\tattached shaders: %u\n",num_attached);
    if(num_attached > 0) {
      GLuint attached[2] = { 0,0 };
      glGetAttachedShaders(program,2,0,attached);
      printf("\t");
      for(size_t i = 0;i < 2;++i) {
	if(attached[i] > 0) {
	  printf("%u ",attached[i]);
	}
      }
      printf("\n");
    }

    GLint nInfo = 0;
    glGetProgramiv(program,GL_INFO_LOG_LENGTH,&nInfo);
    if(nInfo > 0) {
      printf("\tinfo length: %u\n",nInfo);
      char szInfo[nInfo + 1];
      glGetProgramInfoLog(program,nInfo + 1,0,szInfo);
      printf("\tinfo: %s\n",szInfo);
    }
  }
  else {
    printf("%u is not a program\n",program);
  }
}

static void
summarize_program(const char * label,GLuint program)
{
  printf("summary of program %s:\n",label);

  // Report on attributes:
  {
    GLint num_attribs = 0, attrib_name_length = 0;
    glGetProgramiv(program,GL_ACTIVE_ATTRIBUTES,&num_attribs);
    glGetProgramiv(program,GL_ACTIVE_ATTRIBUTE_MAX_LENGTH,&attrib_name_length);
    printf("%u attribs, name max length: %u\n",num_attribs,attrib_name_length);
    char szName[attrib_name_length + 1];

    for(size_t i = 0;i < num_attribs;++i) {
      GLint size = 0;
      GLenum type = 0;
      GLint location = 0;
      glGetActiveAttrib(program,i,attrib_name_length + 1,0,&size,&type,szName);
      location = glGetAttribLocation(program,szName);
      printf("\t%u: %s %u %u %u\n",i,szName,(unsigned int)location,(unsigned int)size,(unsigned int)type);
    }
  }

// Report on uniforms:
  {
    GLint num_uniforms = 0, uniform_name_length = 0;
    glGetProgramiv(program,GL_ACTIVE_UNIFORMS,&num_uniforms);
    glGetProgramiv(program,GL_ACTIVE_UNIFORM_MAX_LENGTH,&uniform_name_length);
    printf("%u uniforms, name max length: %u\n",num_uniforms,uniform_name_length);
    char szName[uniform_name_length + 1];

    for(size_t i = 0;i < num_uniforms;++i) {
      GLint size = 0;
      GLenum type = 0;
      GLint location = 0;
      glGetActiveUniform(program,i,uniform_name_length + 1,0,&size,&type,szName);
      location = glGetUniformLocation(program,szName);
      printf("\t%u: %s %u %u %x\n",i,szName,(unsigned int)location,(unsigned int)size,(unsigned int)type);
    }
  }
}

// quit:
int running = 1, drawing = 1;

static void
eventHandle(u64 status, u64 param, void * userdata) {
  (void)param;
  (void)userdata;
  if(status == SYSUTIL_EXIT_GAME){
    //printf("Quit app requested\n");
    //exit(0);
    running = 0;
  }
  else if(status == SYSUTIL_MENU_OPEN) {
    drawing = 0;
  }
  else if(status == SYSUTIL_MENU_CLOSE) {
    drawing = 1;
  }
  else {
    //printf("Unhandled event: %08llX\n", (unsigned long long int)status);
  }
}

static void
appCleanup()
{
  sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
  printf("Exiting for real.\n");
}


GLuint textures[3] = { 0,0,0 };
GLuint vao = 0;
GLuint buffers[2] = { 0,0 };
GLuint shaders[2] = { 0,0 };
GLuint program = 0;
GLint ProjMatrix_location = -1, TransMatrix_location = -1, vertex_location = -1, tc_location = -1, image_location = -1, gradient_location = -1;

#define DTOR(X) ((X)*0.01745329f)

using namespace Vectormath::Aos;

static Matrix4 P;


static void
cube_init(void)
{
    printf("%s\n", __PRETTY_FUNCTION__);
    P = Matrix4::perspective(DTOR(54.3), 1920.0 / 1080.0, 0.1f, 1000.0f);
    
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    // Set up us the program:
    shaders[0] = glCreateShader(GL_VERTEX_SHADER);
    shaders[1] = glCreateShader(GL_FRAGMENT_SHADER);

    program = glCreateProgram();

    glAttachShader(program, shaders[0]);
    glAttachShader(program, shaders[1]);

    // Supply shader SOURCES!
    const GLchar* shader_srcs[] = { (const GLchar*)texcube_vert, (const GLchar*)texcube_frag };
    GLint shader_srcs_lengths[] = { (int)texcube_vert_len, (int)texcube_frag_len };
    GLint compiled = 0;

    glShaderSource(shaders[0], 1, shader_srcs, shader_srcs_lengths);
    glCompileShader(shaders[0]);

    glGetShaderiv(shaders[0], GL_COMPILE_STATUS, &compiled);
    printf("shader compile status: %i\n", compiled);

    glShaderSource(shaders[1], 1, shader_srcs + 1, shader_srcs_lengths + 1);
    glCompileShader(shaders[1]);

    glGetShaderiv(shaders[1], GL_COMPILE_STATUS, &compiled);
    printf("shader compile status: %i\n", compiled);

    // Link the program for real:
    glLinkProgram(program);

    glValidateProgram(program);

    summarize_program("draw", program);

    vertex_location = glGetAttribLocation(program, "vertex");
    tc_location = glGetAttribLocation(program, "uv");

    ProjMatrix_location = glGetUniformLocation(program, "ProjMatrix");
    TransMatrix_location = glGetUniformLocation(program, "TransMatrix");
    image_location = glGetUniformLocation(program, "image");
    gradient_location = glGetUniformLocation(program, "gradient");

    printf("vertex_location: %i\n", vertex_location);
    printf("tc_location: %i\n", tc_location);
    printf("ProjMatrix_location: %i TransMatrix_location: %i\n",
        ProjMatrix_location, TransMatrix_location);
    printf("image_location: %i gradient_location: %i\n", image_location, gradient_location);

    glUseProgram(program);

    //glUniformMatrix4fv(ProjMatrix_location, 1, GL_FALSE, ProjMatrix.data());
    glUniformMatrix4fv(ProjMatrix_location, 1, GL_FALSE, (float*)&P);
    glUniform1i(image_location, 0);
    glUniform1i(gradient_location, 2);

    // Set up us the vertex data:
    glGenBuffers(2, buffers);

    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 12 /*6 * 4*/ * 5, geometry, GL_STATIC_DRAW);

    glEnableVertexAttribArray(vertex_location);
    glEnableVertexAttribArray(tc_location);
    glVertexAttribPointer(vertex_location, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, 0);
    glVertexAttribPointer(tc_location, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (const GLvoid*)(sizeof(float) * 3));

    client_indices = (GLuint*)malloc(sizeof(GLuint) * 6 * 2 * 3);
    memcpy(client_indices, indices, sizeof(GLuint) * 6 * 2 * 3);

    pngData* nagel_png;
    pngData* gradient_img2;
    nagel_png = new pngData;
    pngLoadFromBuffer(nagel_bin, nagel_bin_size, nagel_png);
    gradient_img2 = new pngData;
    pngLoadFromBuffer(gradient_png, gradient_png_size, gradient_img2);

    glGenTextures(3, textures);

    //
    glActiveTexture(GL_TEXTURE0);

    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA, nagel_png->width, nagel_png->height);
    //glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, nagel_image.width, nagel_image.height, GL_RGBA, GL_UNSIGNED_BYTE, nagel_image.data);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, nagel_png->width, nagel_png->height, GL_RGBA, GL_UNSIGNED_BYTE, nagel_png->bmp_out);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    //
    glActiveTexture(GL_TEXTURE2);

    glBindTexture(GL_TEXTURE_2D, textures[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gradient_img2->width, gradient_img2->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, gradient_img2->bmp_out);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

}



static void
draw_cube()
{
    float rgb[3] = {
      0.5,00.5,0.5
    };

    glClearColor(rgb[0], rgb[1], rgb[2], 1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Matrix4 rotX, rotY;
    Matrix4 modelViewMatrix;
   
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
       
        
        
        rotX = Matrix4::rotationX(DTOR(-26.738));
        rotY = Matrix4::rotationY(DTOR(23.8));

        modelViewMatrix = inverse(Matrix4::identity() * Matrix4::translation(Vector3(1.779f, 2.221f, 4.034f)) * (Matrix4::rotationZ(DTOR(0)) * rotY * rotX));


        glUniformMatrix4fv(TransMatrix_location, 1, GL_FALSE, (float*)&modelViewMatrix);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, client_indices);
    }


}


int
main()
{
  
  printf("%s\n","rsxglcube");
  if (sysModuleLoad(SYSMODULE_PNGDEC) != 0) exit(0);

  //glInitDebug(1024*256,tcp_puts);

  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

  if(dpy != EGL_NO_DISPLAY) {
    
    
    EGLint version0 = 0,version1 = 0;
    EGLBoolean result;
    result = eglInitialize(dpy,&version0,&version1);
    
    if(result) {
      printf("eglInitialize version: %i %i:%i\n",version0,version1,(int)result);
      
      EGLint attribs[] = {
	EGL_RED_SIZE,8,
	EGL_BLUE_SIZE,8,
	EGL_GREEN_SIZE,8,
	EGL_ALPHA_SIZE,8,

	EGL_DEPTH_SIZE,16,
	EGL_NONE
      };
      EGLConfig config;
      EGLint nconfig = 0;
      result = eglChooseConfig(dpy,attribs,&config,1,&nconfig);
      printf("eglChooseConfig:%i %u configs\n",(int)result,nconfig);
      if(nconfig > 0) {
	EGLSurface surface = eglCreateWindowSurface(dpy,config,0,0);
	
	if(surface != EGL_NO_SURFACE) {
	  eglQuerySurface(dpy,surface,EGL_WIDTH,&rsxgltest_width);
	  eglQuerySurface(dpy,surface,EGL_HEIGHT,&rsxgltest_height);

	  printf("eglCreateWindowSurface: %ix%i\n",rsxgltest_width,rsxgltest_height);
	  
	  EGLContext ctx = eglCreateContext(dpy,config,0,0);
	  printf("eglCreateContext: %lu\n",(unsigned long)ctx);
	  
	  if(ctx != EGL_NO_CONTEXT) {
	    atexit(appCleanup);
	    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, eventHandle, NULL);

	    // Initialize:
	    result = eglMakeCurrent(dpy,surface,surface,ctx);

	    if(result == EGL_TRUE) {
	      printf("eglMakeCurrent\n");
          cube_init();
	      
	      while(running) {
		
		
		
		if(drawing) {
            
            draw_cube();
		}
		
		result = eglSwapBuffers(dpy,surface);
		
		EGLint e = eglGetError();
		if(!result) {
		  printf("Swap sync timed-out: %x\n",e);
		  break;
		}
		else {
		  
		  
		  sysUtilCheckCallback();
		}
	      }
	    
          //EXIT HERE
	    }
	    else {
	      printf("eglMakeCurrent failed: %x\n",eglGetError());
	    }

	    result = eglDestroyContext(dpy,ctx);
	    printf("eglDestroyContext:%i\n",(int)result);
	  }
	  else {
	    printf("eglCreateContext failed: %x\n",eglGetError());
	  }
	}
	else {
	  printf("eglCreateWindowSurface failed: %x\n",eglGetError());
	}
      }
      
      result = eglTerminate(dpy);
      printf("eglTerminate:%i\n",(int)result);

      exit(0);
    }
    else {
      printf("eglInitialize failed: %x\n",eglGetError());
    }
  }
  else {
    printf("eglGetDisplay failed: %x\n",eglGetError());
  }

  appCleanup();
    
  return 0;
}
