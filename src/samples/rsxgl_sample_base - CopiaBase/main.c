/*
 * rsxgltest - host code
 *
 * I promise this won't become a rewrite of GLUT. In fact, I plan to switch to SDL soon.
 */

#include <EGL/egl.h>
#define GL3_PROTOTYPES
#define _GNU_SOURCE
#include <GL3/gl3.h>
#include <GL3/rsxgl.h>
#include <GL3/rsxgl3ext.h>

#include <net/net.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>

#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdarg.h>

#include "rsxgl_config.h"
#include <rsx/commands.h>


extern int rsxgltest_draw();
extern void rsxgltest_pad(unsigned int,const padData *);
extern void rsxgltest_exit();

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
static struct gear* gear1, * gear2, * gear3;
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

static struct gear*
create_gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
    GLint teeth, GLfloat tooth_depth);
void perspective(GLfloat* m, GLfloat fovy, GLfloat aspect, GLfloat zNear, GLfloat zFar);

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

static void
gears_init(void)
{
    GLuint v, f, program;
    const char* p;
    char msg[512];

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    /* Compile the vertex shader */
    p = vertex_shader;
    v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &p, NULL);
    glCompileShader(v);
    glGetShaderInfoLog(v, sizeof msg, NULL, msg);
    printf("vertex shader info: %s\n", msg);

    /* Compile the fragment shader */
    p = fragment_shader;
    f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &p, NULL);
    glCompileShader(f);
    glGetShaderInfoLog(f, sizeof msg, NULL, msg);
    printf("fragment shader info: %s\n", msg);

    /* Create and link the shader program */
    program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);

    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "normal");

    glLinkProgram(program);
    glGetProgramInfoLog(program, sizeof msg, NULL, msg);
    printf("info: %s\n", msg);

    summarize_program("rsxglgears", program);

    /* Enable the shaders */
    glUseProgram(program);

    /* Get the locations of the uniforms so we can access them */
    ModelViewProjectionMatrix_location = glGetUniformLocation(program, "ModelViewProjectionMatrix");
    NormalMatrix_location = glGetUniformLocation(program, "NormalMatrix");
    LightSourcePosition_location = glGetUniformLocation(program, "LightSourcePosition");
    MaterialColor_location = glGetUniformLocation(program, "MaterialColor");

    printf("locations: %i %i %i %i\n",
        ModelViewProjectionMatrix_location,
        NormalMatrix_location,
        LightSourcePosition_location,
        MaterialColor_location);

    /* Set the LightSourcePosition uniform which is constant throught the program */
    glUniform4fv(LightSourcePosition_location, 1, LightSourcePosition);

    /* make the gears */
    gear1 = create_gear(1.0, 4.0, 1.0, 20, 0.7);
    gear2 = create_gear(0.5, 2.0, 2.0, 10, 0.7);
    gear3 = create_gear(1.3, 2.0, 0.5, 10, 0.7);
}

/**
 * Handles a new window size or exposure.
 *
 * @param width the window width
 * @param height the window height
 */
static void
gears_reshape(int width, int height)
{
    /* Update the projection matrix */
    perspective(ProjectionMatrix, 60.0, width / (float)height, 1.0, 1024.0);

    /* Set the viewport */
    glViewport(0, 0, (GLint)width, (GLint)height);
}



static void
gears_idle(void)
{
//    static int frames = 0;
//    static double tRot0 = -1.0, tRate0 = -1.0;
//    double dt, t = rsxgltest_elapsed_time /*eglutGet(EGLUT_ELAPSED_TIME) / 1000.0*/;
//
//    if (tRot0 < 0.0)
//        tRot0 = t;
//    dt = t - tRot0;
//    tRot0 = t;
//
//    /* advance rotation for next frame */
//    angle += 70.0 * dt;  /* 70 degrees per second */
//    if (angle > 3600.0)
//        angle -= 3600.0;
//
//#if 0
//    eglutPostRedisplay();
//#endif
//    frames++;
//
//    if (tRate0 < 0.0)
//        tRate0 = t;
//    if (t - tRate0 >= 5.0) {
//        GLfloat seconds = t - tRate0;
//        GLfloat fps = frames / seconds;
//        printf("%d frames in %3.1f seconds = %6.3f FPS\n", frames, seconds,
//            fps);
//        tRate0 = t;
//        frames = 0;
//    }
}




#define STRIPS_PER_TOOTH 7
#define VERTICES_PER_TOOTH 34
#define GEAR_VERTEX_STRIDE 6

/**
 * Struct describing the vertices in triangle strip
 */
struct vertex_strip {
    /** The first vertex in the strip */
    GLint first;
    /** The number of consecutive vertices in the strip after the first */
    GLint count;
};

/* Each vertex consist of GEAR_VERTEX_STRIDE GLfloat attributes */
typedef GLfloat GearVertex[GEAR_VERTEX_STRIDE];

/**
 * Struct representing a gear.
 */
struct gear {
    /** The array of vertices comprising the gear */
    GearVertex* vertices;
    /** The number of vertices comprising the gear */
    int nvertices;
    /** The array of triangle strips comprising the gear */
    struct vertex_strip* strips;
    /** The number of triangle strips comprising the gear */
    int nstrips;
    /** The Vertex Buffer Object holding the vertices in the graphics card */
    GLuint vbo;
};



/**
 * Fills a gear vertex.
 *
 * @param v the vertex to fill
 * @param x the x coordinate
 * @param y the y coordinate
 * @param z the z coortinate
 * @param n pointer to the normal table
 *
 * @return the operation error code
 */
static GearVertex*
vert(GearVertex* v, GLfloat x, GLfloat y, GLfloat z, GLfloat n[3])
{
    v[0][0] = x;
    v[0][1] = y;
    v[0][2] = z;
    v[0][3] = n[0];
    v[0][4] = n[1];
    v[0][5] = n[2];

    return v + 1;
}

/**
 *  Create a gear wheel.
 *
 *  @param inner_radius radius of hole at center
 *  @param outer_radius radius at center of teeth
 *  @param width width of gear
 *  @param teeth number of teeth
 *  @param tooth_depth depth of tooth
 *
 *  @return pointer to the constructed struct gear
 */
static struct gear*
create_gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
    GLint teeth, GLfloat tooth_depth)
{
    GLfloat r0, r1, r2;
    GLfloat da;
    GearVertex* v;
    struct gear* gear;
    double s[5], c[5];
    GLfloat normal[3];
    int cur_strip = 0;
    int i;

    /* Allocate memory for the gear */
    gear = malloc(sizeof * gear);
    if (gear == NULL)
        return NULL;

    /* Calculate the radii used in the gear */
    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2.0;
    r2 = outer_radius + tooth_depth / 2.0;

    da = 2.0 * M_PI / teeth / 4.0;

    /* Allocate memory for the triangle strip information */
    gear->nstrips = STRIPS_PER_TOOTH * teeth;
    gear->strips = calloc(gear->nstrips, sizeof(*gear->strips));

    /* Allocate memory for the vertices */
    gear->vertices = calloc(VERTICES_PER_TOOTH * teeth, sizeof(*gear->vertices));
    v = gear->vertices;

    for (i = 0; i < teeth; i++) {
        /* Calculate needed sin/cos for varius angles */
        sincos(i * 2.0 * M_PI / teeth, &s[0], &c[0]);
        sincos(i * 2.0 * M_PI / teeth + da, &s[1], &c[1]);
        sincos(i * 2.0 * M_PI / teeth + da * 2, &s[2], &c[2]);
        sincos(i * 2.0 * M_PI / teeth + da * 3, &s[3], &c[3]);
        sincos(i * 2.0 * M_PI / teeth + da * 4, &s[4], &c[4]);

        /* A set of macros for making the creation of the gears easier */
#define  GEAR_POINT(r, da) { (r) * c[(da)], (r) * s[(da)] }
#define  SET_NORMAL(x, y, z) do { \
   normal[0] = (x); normal[1] = (y); normal[2] = (z); \
} while(0)

#define  GEAR_VERT(v, point, sign) vert((v), p[(point)].x, p[(point)].y, (sign) * width * 0.5, normal)

#define START_STRIP do { \
   gear->strips[cur_strip].first = v - gear->vertices; \
} while(0);

#define END_STRIP do { \
   int _tmp = (v - gear->vertices); \
   gear->strips[cur_strip].count = _tmp - gear->strips[cur_strip].first; \
   cur_strip++; \
} while (0)

#define QUAD_WITH_NORMAL(p1, p2) do { \
   SET_NORMAL((p[(p1)].y - p[(p2)].y), -(p[(p1)].x - p[(p2)].x), 0); \
   v = GEAR_VERT(v, (p1), -1); \
   v = GEAR_VERT(v, (p1), 1); \
   v = GEAR_VERT(v, (p2), -1); \
   v = GEAR_VERT(v, (p2), 1); \
} while(0)

        struct point {
            GLfloat x;
            GLfloat y;
        };

        /* Create the 7 points (only x,y coords) used to draw a tooth */
        struct point p[7] = {
           GEAR_POINT(r2, 1), // 0
           GEAR_POINT(r2, 2), // 1
           GEAR_POINT(r1, 0), // 2
           GEAR_POINT(r1, 3), // 3
           GEAR_POINT(r0, 0), // 4
           GEAR_POINT(r1, 4), // 5
           GEAR_POINT(r0, 4), // 6
        };

        /* Front face */
        START_STRIP;
        SET_NORMAL(0, 0, 1.0);
        v = GEAR_VERT(v, 0, +1);
        v = GEAR_VERT(v, 1, +1);
        v = GEAR_VERT(v, 2, +1);
        v = GEAR_VERT(v, 3, +1);
        v = GEAR_VERT(v, 4, +1);
        v = GEAR_VERT(v, 5, +1);
        v = GEAR_VERT(v, 6, +1);
        END_STRIP;

        /* Inner face */
        START_STRIP;
        QUAD_WITH_NORMAL(4, 6);
        END_STRIP;

        /* Back face */
        START_STRIP;
        SET_NORMAL(0, 0, -1.0);
        v = GEAR_VERT(v, 6, -1);
        v = GEAR_VERT(v, 5, -1);
        v = GEAR_VERT(v, 4, -1);
        v = GEAR_VERT(v, 3, -1);
        v = GEAR_VERT(v, 2, -1);
        v = GEAR_VERT(v, 1, -1);
        v = GEAR_VERT(v, 0, -1);
        END_STRIP;

        /* Outer face */
        START_STRIP;
        QUAD_WITH_NORMAL(0, 2);
        END_STRIP;

        START_STRIP;
        QUAD_WITH_NORMAL(1, 0);
        END_STRIP;

        START_STRIP;
        QUAD_WITH_NORMAL(3, 1);
        END_STRIP;

        START_STRIP;
        QUAD_WITH_NORMAL(5, 3);
        END_STRIP;
    }

    gear->nvertices = (v - gear->vertices);

    /* Store the vertices in a vertex buffer object (VBO) */
    glGenBuffers(1, &gear->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gear->vbo);
    glBufferData(GL_ARRAY_BUFFER, gear->nvertices * sizeof(GearVertex),
        gear->vertices, GL_STATIC_DRAW);

    return gear;
}

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
void perspective(GLfloat* m, GLfloat fovy, GLfloat aspect, GLfloat zNear, GLfloat zFar)
{
    GLfloat tmp[16];
    identity(tmp);

    double sine, cosine, cotangent, deltaZ;
    GLfloat radians = fovy / 2 * M_PI / 180;

    deltaZ = zFar - zNear;
    sincos(radians, &sine, &cosine);

    if ((deltaZ == 0) || (sine == 0) || (aspect == 0))
        return;

    cotangent = cosine / sine;

    tmp[0] = cotangent / aspect;
    tmp[5] = cotangent;
    tmp[10] = -(zFar + zNear) / deltaZ;
    tmp[11] = -1;
    tmp[14] = -2 * zNear * zFar / deltaZ;
    tmp[15] = 0;

    memcpy(m, tmp, sizeof(tmp));
}


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
draw_gear(struct gear* gear, GLfloat* transform,
    GLfloat x, GLfloat y, GLfloat angle, const GLfloat color[4])
{
    GLfloat model_view[16];
    GLfloat normal_matrix[16];
    GLfloat model_view_projection[16];

    /* Translate and rotate the gear */
    memcpy(model_view, transform, sizeof(model_view));
    translate(model_view, x, y, 0);
    //rotate(model_view, 2 * M_PI * angle / 360.0, 0, 0, 1);

    /* Create and set the ModelViewProjectionMatrix */
    memcpy(model_view_projection, ProjectionMatrix, sizeof(model_view_projection));
    multiply(model_view_projection, model_view);

    glUniformMatrix4fv(ModelViewProjectionMatrix_location, 1, GL_FALSE,
        model_view_projection);

    /*
     * Create and set the NormalMatrix. It's the inverse transpose of the
     * ModelView matrix.
     */
    memcpy(normal_matrix, model_view, sizeof(normal_matrix));
    invert(normal_matrix);
    transpose(normal_matrix);
    glUniformMatrix4fv(NormalMatrix_location, 1, GL_FALSE, normal_matrix);

    /* Set the gear color */
    glUniform4fv(MaterialColor_location, 1, color);

    /* Set the vertex buffer object to use */
    glBindBuffer(GL_ARRAY_BUFFER, gear->vbo);

    /* Set up the position of the attributes in the vertex buffer object */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        6 * sizeof(GLfloat), NULL);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
        6 * sizeof(GLfloat), (GLfloat*)0 + 3);

    /* Enable the attributes */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    /* Draw the triangle strips that comprise the gear */
    int n;
    for (n = 0; n < gear->nstrips; n++)
        glDrawArrays(GL_TRIANGLE_STRIP, gear->strips[n].first, gear->strips[n].count);


    /* Disable the attributes */
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
}


/**
 * Draws the gears.
 */
static void
gears_draw(void)
{
    const static GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 };
    const static GLfloat green[4] = { 0.0, 0.8, 0.2, 1.0 };
    const static GLfloat blue[4] = { 0.2, 0.2, 1.0, 1.0 };
    GLfloat transform[16];
    identity(transform);

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Translate and rotate the view */
    translate(transform, 0, 0, -20);
    //rotate(transform, 2 * M_PI * view_rot[0] / 360.0, 1, 0, 0);
    //rotate(transform, 2 * M_PI * view_rot[1] / 360.0, 0, 1, 0);
    //rotate(transform, 2 * M_PI * view_rot[2] / 360.0, 0, 0, 1);

    /* Draw the gears */
    draw_gear(gear1, transform, -3.0, -2.0, angle, red);
    draw_gear(gear2, transform, 3.1, -2.0, -2 * angle - 9.0, green);
    draw_gear(gear3, transform, -3.1, 4.2, -2 * angle - 25.0, blue);
}

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
          gears_init();
          gears_reshape(rsxgltest_width, rsxgltest_height);
	      
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
            gears_idle();
            gears_draw();
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
