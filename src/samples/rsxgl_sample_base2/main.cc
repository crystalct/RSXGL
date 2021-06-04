/*
 * rsxgltest - host code
 *
 * I promise this won't become a rewrite of GLUT. In fact, I plan to switch to SDL soon.
 */

#include <EGL/egl.h>
#define GL3_PROTOTYPES
//#define _GNU_SOURCE
#include <GL3/gl3.h>
#include <GL3/rsxgl.h>
#include <GL3/rsxgl3ext.h>
#include <stddef.h>
#include <net/net.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>
#include "sine_wave.h"
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "math3d.h"
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <Eigen/Geometry>

#include <stdarg.h>

#include "rsxgl_config.h"
#include <rsx/commands.h>

#include "texcube_vert.h"
#include "texcube_frag.h"


const float geometry[] = {
       0,0,0,1, 0,
   1,0,0,1, 1,
   1,1,0,0, 1,
   0,1,0,0, 0,
   1,0,1,1, 0,
   1,1,1,0, 0,
   0,1,1,0, 1,
   0,0,1,1, 1,
   0,1,1,1, 0,
   0,1,0,1, 1,
   1,0,1,0, 1,
   1,0,0,0, 0
};

struct sine_wave_t rgb_waves[3] = {
  { 0.5f,
    0.5f,
    1.0f
  },
  { 0.5f,
    0.5f,
    1.5f
  },
  { 0.5f,
    0.5f,
    2.5f
  }
};

struct sine_wave_t xyz_waves[3] = {
  { 0.5f,
    0.5f,
    1.0f / 4.0f
  },
  { 0.5f,
    0.5f,
    1.5f / 4.0f
  },
  { 0.5f,
    0.5f,
    2.5f / 4.0f
  }
};


GLuint* client_indices = 0;

const GLuint indices[] = {

      0,1,2,   0,2,3,   1,4,5,   1,5,2,   4,7,6,	 4,6,5,
                            7,0,3,   7,3,6,   9,2,5,   9,5,8,   0,10,11,   0,7,10
};
// Test program might want to use these:
int rsxgltest_width = 0, rsxgltest_height = 0;
float rsxgltest_elapsed_time = 0, rsxgltest_last_time = 0, rsxgltest_delta_time = 0;

// Configure these (especially the IP) to your own setup.
// Use netcat to receive the results on your PC:
// TCP: nc -l -p 4000
// UDP: nc -u -l -p 4000
// For some versions of netcat the -p option may need to be removed.
//
//#define TESTIP		"192.168.1.7"
//#define TESTIP          "192.168.1.115"
//#define TESTPORT	9000


int sock = 0;
/** The view rotation [x, y, z] */
static GLfloat view_rot[3] = { 20.0, 30.0, 0.0 };
/** The gears */
//static struct gear* gear1; // , * gear2, * gear3;
/** The current gear rotation angle */
static GLfloat angle = 0.0;
/** The location of the shader uniforms */
static GLuint ModelViewProjectionMatrix_location,
NormalMatrix_location,
LightSourcePosition_location,
MaterialColor_location;
/** The projection matrix */
static GLfloat ProjectionMatrix[16];
/** The direction of the directional light for the scene */
static const GLfloat LightSourcePosition[4] = { 5.0, 5.0, 10.0, 1.0 };

//static struct gear*
//create_gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
//    GLint teeth, GLfloat tooth_depth);


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
  //tcp_exit();
  //netDeinitialize();
  printf("Exiting for real.\n");
}

/* Convenience macros for operations on timevals.
   NOTE: `timercmp' does not work for >= or <=.  */
//#define	timerisset(tvp)		((tvp)->tv_sec || (tvp)->tv_usec)
//#define	timerclear(tvp)		((tvp)->tv_sec = (tvp)->tv_usec = 0)
//#define	timercmp(a, b, CMP) 						      \
//  (((a)->tv_sec == (b)->tv_sec) ? 					      \
//   ((a)->tv_usec CMP (b)->tv_usec) : 					      \
//   ((a)->tv_sec CMP (b)->tv_sec))
//#define	timeradd(a, b, result)						      \
//  do {									      \
//    (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;			      \
//    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;			      \
//    if ((result)->tv_usec >= 1000000)					      \
//      {									      \
//	++(result)->tv_sec;						      \
//	(result)->tv_usec -= 1000000;					      \
//      }									      \
//  } while (0)
//#define	timersub(a, b, result)						      \
//  do {									      \
//    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;			      \
//    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;			      \
//    if ((result)->tv_usec < 0) {					      \
//      --(result)->tv_sec;						      \
//      (result)->tv_usec += 1000000;					      \
//    }									      \
//  } while (0)

static const char vertex_shader[] =
"attribute vec3 position;\n"
"attribute vec3 normal;\n"
"\n"
"uniform mat4 ModelViewProjectionMatrix;\n"
"uniform mat4 NormalMatrix;\n"
"uniform vec4 LightSourcePosition;\n"
"uniform vec4 MaterialColor;\n"
"\n"
"varying vec4 Color;\n"
"\n"
"void main(void)\n"
"{\n"
"    // Transform the normal to eye coordinates\n"
"    vec3 N = normalize(vec3(NormalMatrix * vec4(normal, 1.0)));\n"
"\n"
"    // The LightSourcePosition is actually its direction for directional light\n"
"    vec3 L = normalize(LightSourcePosition.xyz);\n"
"\n"
"    // Multiply the diffuse value by the vertex color (which is fixed in this case)\n"
"    // to get the actual color that we will use to draw this vertex with\n"
"    float diffuse = max(dot(N, L), 0.0);\n"
"    Color = diffuse * MaterialColor;\n"
"\n"
"    // Transform the position to clip coordinates\n"
"    gl_Position = ModelViewProjectionMatrix * vec4(position, 1.0);\n"
"}";

static const char fragment_shader[] =
"//precision mediump float;\n"
"varying vec4 Color;\n"
"\n"
"void main(void)\n"
"{\n"
"    gl_FragColor = Color;\n"
"}";

GLuint vao = 0;
GLuint buffers[2] = { 0,0 };
GLuint shaders[2] = { 0,0 };
GLuint program = 0;
GLint ProjMatrix_location = -1, TransMatrix_location = -1, vertex_location = -1, tc_location = -1, image_location = -1, gradient_location = -1;

#define DTOR(X) ((X)*0.01745329f)
#define RTOD(d) ((d)*57.295788f)

Eigen::Projective3f ProjMatrix(perspective(DTOR(54.3), 1920.0 / 1080.0, 0.1, 1000.0));

Eigen::Affine3f ViewMatrixInv =
Eigen::Affine3f(Eigen::Affine3f::Identity() *
    (
        Eigen::Translation3f(1.779, 2.221, 4.034) *
        (
            Eigen::AngleAxisf(DTOR(0), Eigen::Vector3f::UnitZ()) *
            Eigen::AngleAxisf(DTOR(23.8), Eigen::Vector3f::UnitY()) *
            Eigen::AngleAxisf(DTOR(-26.738), Eigen::Vector3f::UnitX())
            )
        )
).inverse();

static void
cube_init(void)
{
    printf("%s\n", __PRETTY_FUNCTION__);

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
    GLint shader_srcs_lengths[] = { texcube_vert_len, texcube_frag_len };
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

    glUniformMatrix4fv(ProjMatrix_location, 1, GL_FALSE, ProjMatrix.data());

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

}

/**
 * Handles a new window size or exposure.
 *
 * @param width the window width
 * @param height the window height
 */
//static void
//cube_reshape(int width, int height)
//{
//    /* Update the projection matrix */
//    perspective(ProjectionMatrix, 60.0, width / (float)height, 1.0, 1024.0);
//
//    /* Set the viewport */
//    glViewport(0, 0, (GLint)width, (GLint)height);
//}





/**
 * Struct describing the vertices in triangle strip
 */
struct vertex_strip {
    /** The first vertex in the strip */
    GLint first;
    /** The number of consecutive vertices in the strip after the first */
    GLint count;
};





/**
 * Multiplies two 4x4 matrices.
 *
 * The result is stored in matrix m.
 *
 * @param m the first matrix to multiply
 * @param n the second matrix to multiply
 */
static void
multiply(GLfloat* m, const GLfloat* n)
{
    GLfloat tmp[16];
    const GLfloat* row, * column;
    div_t d;
    int i, j;

    for (i = 0; i < 16; i++) {
        tmp[i] = 0;
        d = div(i, 4);
        row = n + d.quot * 4;
        column = m + d.rem;
        for (j = 0; j < 4; j++)
            tmp[i] += row[j] * column[j * 4];
    }
    memcpy(m, &tmp, sizeof tmp);
}

/**
 * Rotates a 4x4 matrix.
 *
 * @param[in,out] m the matrix to rotate
 * @param angle the angle to rotate
 * @param x the x component of the direction to rotate to
 * @param y the y component of the direction to rotate to
 * @param z the z component of the direction to rotate to
 */
//static void
//rotate(GLfloat* m, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
//{
//    double s, c;
//
//    sincos(angle, &s, &c);
//    GLfloat r[16] = {
//       x * x * (1 - c) + c,     y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0,
//       x * y * (1 - c) - z * s, y * y * (1 - c) + c,     y * z * (1 - c) + x * s, 0,
//       x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c,     0,
//       0, 0, 0, 1
//    };
//
//    multiply(m, r);
//}


/**
 * Translates a 4x4 matrix.
 *
 * @param[in,out] m the matrix to translate
 * @param x the x component of the direction to translate to
 * @param y the y component of the direction to translate to
 * @param z the z component of the direction to translate to
 */
static void
translate(GLfloat* m, GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat t[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 };

    multiply(m, t);
}

/**
 * Creates an identity 4x4 matrix.
 *
 * @param m the matrix make an identity matrix
 */
static void
identity(GLfloat* m)
{
    GLfloat t[16] = {
       1.0, 0.0, 0.0, 0.0,
       0.0, 1.0, 0.0, 0.0,
       0.0, 0.0, 1.0, 0.0,
       0.0, 0.0, 0.0, 1.0,
    };

    memcpy(m, t, sizeof(t));
}

/**
 * Transposes a 4x4 matrix.
 *
 * @param m the matrix to transpose
 */
static void
transpose(GLfloat* m)
{
    GLfloat t[16] = {
       m[0], m[4], m[8],  m[12],
       m[1], m[5], m[9],  m[13],
       m[2], m[6], m[10], m[14],
       m[3], m[7], m[11], m[15] };

    memcpy(m, t, sizeof(t));
}

/**
 * Inverts a 4x4 matrix.
 *
 * This function can currently handle only pure translation-rotation matrices.
 * Read http://www.gamedev.net/community/forums/topic.asp?topic_id=425118
 * for an explanation.
 */
static void
invert(GLfloat* m)
{
    GLfloat t[16];
    identity(t);

    // Extract and invert the translation part 't'. The inverse of a
    // translation matrix can be calculated by negating the translation
    // coordinates.
    t[12] = -m[12]; t[13] = -m[13]; t[14] = -m[14];

    // Invert the rotation part 'r'. The inverse of a rotation matrix is
    // equal to its transpose.
    m[12] = m[13] = m[14] = 0;
    transpose(m);

    // inv(m) = inv(r) * inv(t)
    multiply(m, t);
}

/**
 * Calculate a perspective projection transformation.
 *
 * @param m the matrix to save the transformation in
 * @param fovy the field of view in the y direction
 * @param aspect the view aspect ratio
 * @param zNear the near clipping plane
 * @param zFar the far clipping plane
 */


/**
 * Draws a gear.
 *
 * @param gear the gear to draw
 * @param transform the current transformation matrix
 * @param x the x position to draw the gear at
 * @param y the y position to draw the gear at
 * @param angle the rotation angle of the gear
 * @param color the color of the gear
 */
static void
draw_cube()
{
    float rgb[3] = {
      1,0,0
    };

    glClearColor(rgb[0], rgb[1], rgb[2], 1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float xyz[3] = {
      compute_sine_wave(xyz_waves,rsxgltest_elapsed_time),
      compute_sine_wave(xyz_waves + 1,rsxgltest_elapsed_time),
      compute_sine_wave(xyz_waves + 2,rsxgltest_elapsed_time)
    };

    Eigen::Affine3f rotmat =
        Eigen::Affine3f::Identity() *
        Eigen::AngleAxisf(DTOR(xyz[2]) * 360.0f, Eigen::Vector3f::UnitZ()) *
        Eigen::AngleAxisf(DTOR(xyz[1]) * 360.0f, Eigen::Vector3f::UnitY()) *
        Eigen::AngleAxisf(DTOR(xyz[0]) * 360.0f, Eigen::Vector3f::UnitX());

    {
        //glActiveTexture(GL_TEXTURE0);
        //glBindTexture(GL_TEXTURE_2D, textures[0]);
        //glUniform1i(image_location,0);

        Eigen::Affine3f modelview = ViewMatrixInv * (Eigen::Affine3f::Identity() * Eigen::Translation3f(-5, 0, 0) * rotmat * Eigen::UniformScaling< float >(3.0));
        //Eigen::Affine3f modelview = ViewMatrixInv;
        //Eigen::Affine3f modelview(Eigen::Affine3f::Identity());
        glUniformMatrix4fv(TransMatrix_location, 1, GL_FALSE, modelview.data());

        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, client_indices);
    }

    {
        //glActiveTexture(GL_TEXTURE0);
        //glBindTexture(GL_TEXTURE_2D, textures[1]);
        //glUniform1i(image_location,1);

        Eigen::Affine3f modelview = ViewMatrixInv * (Eigen::Affine3f::Identity() * Eigen::Translation3f(5, 0, 0) * rotmat * Eigen::UniformScaling< float >(3.0));
        //Eigen::Affine3f modelview = ViewMatrixInv;
        //Eigen::Affine3f modelview(Eigen::Affine3f::Identity());
        glUniformMatrix4fv(TransMatrix_location, 1, GL_FALSE, modelview.data());

        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, client_indices);
    }
}


/**
 * Draws the gears.
 */

// rsxgltest stuff goes here - replaces glut, eglut, etc.

char* rsxgltest_name = "rsxglgears";


int
main(int argc, const char ** argv)
{
  //netInitialize();
  //tcp_init();
  printf("%s\n",rsxgltest_name);

  //glInitDebug(1024*256,tcp_puts);

  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

  if(dpy != EGL_NO_DISPLAY) {
    // convert to a timeval structure:
    /*const float ft = 1.0f / 60.0f;
    float ft_integral, ft_fractional;
    ft_fractional = modff(ft,&ft_integral);
    struct timeval frame_time = { 0,0 };
    frame_time.tv_sec = (int)ft_integral;
    frame_time.tv_usec = (int)(ft_fractional * 1.0e6);*/
    
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
	    
	    /*struct timeval start_time, current_time;
	    struct timeval timeout_time = {
	      .tv_sec = 6,
	      .tv_usec = 0
	    };*/

	    // Initialize:
	    result = eglMakeCurrent(dpy,surface,surface,ctx);

	    if(result == EGL_TRUE) {
	      printf("eglMakeCurrent\n");
          cube_init();
          //gears_reshape(rsxgltest_width, rsxgltest_height);
	      
	      //gettimeofday(&start_time,0);
	      rsxgltest_last_time = 0.0f;
	      
	      while(running) {
		/*gettimeofday(&current_time,0);
		
		struct timeval elapsed_time;
		timersub(&current_time,&start_time,&elapsed_time);
		rsxgltest_elapsed_time = ((float)(elapsed_time.tv_sec)) + ((float)(elapsed_time.tv_usec) / 1.0e6f);
		rsxgltest_delta_time = rsxgltest_elapsed_time - rsxgltest_last_time;
		
		rsxgltest_last_time = rsxgltest_elapsed_time;*/
		
		//result = eglMakeCurrent(dpy,surface,surface,ctx);
		
		
		if(drawing) {
            //gears_idle();
            draw_cube();
		}
		
		result = eglSwapBuffers(dpy,surface);
		
		EGLint e = eglGetError();
		if(!result) {
		  printf("Swap sync timed-out: %x\n",e);
		  break;
		}
		else {
		  /*struct timeval t, elapsed_time;
		  gettimeofday(&t,0);
		  timersub(&t,&current_time,&elapsed_time);
		  
		  if(timercmp(&elapsed_time,&frame_time,<)) {
		    struct timeval sleep_time;
		    timersub(&frame_time,&elapsed_time,&sleep_time);
		    usleep((sleep_time.tv_sec * 1e6) + sleep_time.tv_usec);
		  }*/
		  
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

#include "sine_wave.c"