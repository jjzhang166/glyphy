/*
 * Copyright 2012 Google, Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Google Author(s): Behdad Esfahbod, Maysum Panju, Wojciech Baranowski
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "demo-view.h"

extern "C" {
#include "trackball.h"
}

#include <sys/time.h>


struct demo_view_t {
  unsigned int   refcount;

  demo_state_t *st;

  /* Output */
  GLint vsync;
  glyphy_bool_t srgb;

  /* Mouse handling */
  int buttons;
  int modifiers;
  bool dragged;
  bool click_handled;
  double beginx, beginy;
  double lastx, lasty, lastt;
  double dx,dy, dt;

  /* Transformation */
  float quat[4];
  float dquat[4];
  double scale;
  glyphy_point_t translate;
  double perspective;

  /* Animation */
  bool animate;
  int num_frames;
  long fps_start_time;
  long last_frame_time;
  bool has_fps_timer;
};

demo_view_t *static_vu;

demo_view_t *
demo_view_create (demo_state_t *st)
{
  demo_view_t *vu = (demo_view_t *) calloc (1, sizeof (demo_view_t));
  vu->refcount = 1;

  vu->st = st;
  demo_view_reset (vu);

  assert (!static_vu);
  static_vu = vu;

  return vu;
}

demo_view_t *
demo_view_reference (demo_view_t *vu)
{
  if (vu) vu->refcount++;
  return vu;
}

void
demo_view_destroy (demo_view_t *vu)
{
  if (!vu || --vu->refcount)
    return;

  assert (static_vu == vu);
  static_vu = NULL;

  free (vu);
}


#define ANIMATION_SPEED .01
void
demo_view_reset (demo_view_t *vu)
{
  vu->perspective = 4;
  vu->scale = 1;
  vu->translate.x = vu->translate.y = 0;
  trackball (vu->quat , 0.0, 0.0, 0.0, 0.0);
  float a[3] = {0, 0, 1};
  axis_to_quat (a, ANIMATION_SPEED, vu->dquat);
}

static void
demo_view_scale_gamma_adjust (demo_view_t *vu, double factor)
{
  demo_state_t *st = vu->st;
  SET_UNIFORM (u_gamma_adjust, clamp (st->u_gamma_adjust * factor, .1, 10.));
}

static void
demo_view_scale_contrast (demo_view_t *vu, double factor)
{
  demo_state_t *st = vu->st;
  SET_UNIFORM (u_contrast, clamp (st->u_contrast * factor, .1, 10.));
}

static void
demo_view_scale_perspective (demo_view_t *vu, double factor)
{
  vu->perspective = clamp (vu->perspective * factor, .01, 100.);
}


static void
demo_view_scale (demo_view_t *vu, double factor)
{
  vu->scale *= factor;
}

static void
demo_view_translate (demo_view_t *vu, double dx, double dy)
{
  vu->translate.x += dx / vu->scale;
  vu->translate.y += dy / vu->scale;
}

static void
demo_view_apply_transform (demo_view_t *vu)
{
  int viewport[4];
  glGetIntegerv (GL_VIEWPORT, viewport);
  GLint width  = viewport[2];
  GLint height = viewport[3];

  // View transform
  glScaled (vu->scale, vu->scale, 1);
  glTranslated (vu->translate.x, vu->translate.y, 0);

  // Perspective
  {
    double d = std::max (width, height);
    double near = d / vu->perspective;
    double far = near + d;
    double factor = near / (2 * near + d);
    glFrustum (-width * factor, width * factor, -height * factor, height * factor, near, far);
    glTranslated (0, 0, -(near + d * .5));
  }

  // Rotate
  float m[4][4];
  build_rotmatrix (m, vu->quat);
  glMultMatrixf(&m[0][0]);

  // Fix 'up'
  glScaled (1, -1, 1);
}


/* return current time in milli-seconds */
static long
current_time (void)
{
   struct timeval tv;
   struct timezone tz;
   (void) gettimeofday(&tv, &tz);
   return (long) tv.tv_sec * 1000 + (long) tv.tv_usec / 1000;
}

static void
next_frame (demo_view_t *vu)
{
  // TODO rotate depending on time elapsed (current_time () - vu->last_frame_time)
  add_quats (vu->dquat, vu->quat, vu->quat);

  vu->num_frames++;
  glutPostRedisplay ();
}

static void
timed_step (int ms)
{
  demo_view_t *vu = static_vu;
  if (vu->animate) {
    glutTimerFunc (ms, timed_step, ms);
    next_frame (vu);
  }
}

static void
idle_step (void)
{
  demo_view_t *vu = static_vu;
  if (vu->animate) {
    glutIdleFunc (idle_step);
    next_frame (vu);
  }
}

static void
print_fps (int ms)
{
  demo_view_t *vu = static_vu;
  if (vu->animate) {
    glutTimerFunc (ms, print_fps, ms);
    long t = current_time ();
    printf ("%gfps\n", vu->num_frames * 1000. / (t - vu->fps_start_time));
    vu->num_frames = 0;
    vu->fps_start_time = t;
  } else
    vu->has_fps_timer = false;
}

static void
start_animation (demo_view_t *vu)
{
  vu->num_frames = 0;
  vu->last_frame_time = vu->fps_start_time = current_time ();
  //glutTimerFunc (1000/60, timed_step, 1000/60);
  glutIdleFunc (idle_step);
  if (!vu->has_fps_timer) {
    vu->has_fps_timer = true;
    glutTimerFunc (5000, print_fps, 5000);
  }
}

static void
demo_view_toggle_animation (demo_view_t *vu)
{
  vu->animate = !vu->animate;
  if (vu->animate)
    start_animation (vu);
}


static void
demo_view_toggle_vsync (demo_view_t *vu)
{
  vu->vsync = !vu->vsync;
  printf ("Setting vsync %s.\n", vu->vsync ? "on" : "off");
#if defined(__APPLE__)
  CGLSetParameter(CGLGetCurrentContext(), kCGLCPSwapInterval, &vu->vsync);
#elif defined(_WIN32)
  if (glewIsSupported ("WGL_EXT_swap_control"))
    wglSwapIntervalEXT (vu->vsync);
  else
    printf ("WGL_EXT_swal_control not supported; failed to set vsync\n");
#else
  if (glxewIsSupported ("GLX_SGI_swap_control"))
    glXSwapIntervalSGI (vu->vsync);
  else
    printf ("GLX_SGI_swap_control not supported; failed to set vsync\n");
#endif
}

static void
demo_view_toggle_srgb (demo_view_t *vu)
{
  vu->srgb = !vu->srgb;
  printf ("Setting sRGB framebuffer %s.\n", vu->srgb ? "on" : "off");
  if (glewIsSupported ("GL_ARB_framebuffer_sRGB") || glewIsSupported ("GL_EXT_framebuffer_sRGB")) {
    if (vu->srgb)
      glEnable (GL_FRAMEBUFFER_SRGB);
    else
      glDisable (GL_FRAMEBUFFER_SRGB);
  } else
    printf ("No sRGB framebuffer extension found; failed to set sRGB framebuffer\n");

}

static void
demo_view_toggle_debug (demo_view_t *vu)
{
  demo_state_t *st = vu->st;
  SET_UNIFORM (u_debug, 1 - st->u_debug);
}

static void
demo_view_next_smoothfunc (demo_view_t *vu)
{
  demo_state_t *st = vu->st;
  SET_UNIFORM (u_smoothfunc, ((int) st->u_smoothfunc + 1) % 3);
}


void
demo_view_reshape_func (demo_view_t *vu, int width, int height)
{
  glViewport (0, 0, width, height);
  glutPostRedisplay ();
}

#define STEP 1.05
void
demo_view_keyboard_func (demo_view_t *vu, unsigned char key, int x, int y)
{
  switch (key)
  {
    case '\033':
    case 'q':
      exit (0);
      break;

    case ' ':
      demo_view_toggle_animation (vu);
      break;
    case 'v':
      demo_view_toggle_vsync (vu);
      break;

    case 'f':
      glutFullScreen ();
      break;

    case 'd':
      demo_view_toggle_debug (vu);
      break;

    case 's':
      demo_view_next_smoothfunc (vu);
      break;
    case 'a':
      demo_view_scale_contrast (vu, STEP);
      break;
    case 'z':
      demo_view_scale_contrast (vu, 1. / STEP);
      break;
    case 'g':
      demo_view_scale_gamma_adjust (vu, STEP);
      break;
    case 'b':
      demo_view_scale_gamma_adjust (vu, 1. / STEP);
      break;
    case 'c':
      demo_view_toggle_srgb (vu);
      break;

    case '=':
      demo_view_scale (vu, STEP);
      break;
    case '-':
      demo_view_scale (vu, 1. / STEP);
      break;

    case 'k': 
      demo_view_translate (vu, 0, -.1);
      break;
    case 'j':
      demo_view_translate (vu, 0, +.1);
      break;
    case 'h':
      demo_view_translate (vu, +.1, 0);
      break;
    case 'l':
      demo_view_translate (vu, -.1, 0);
      break;

    case 'r':
      demo_view_reset (vu);
#define WINDOW_W 700
#define WINDOW_H 700
      glutReshapeWindow (WINDOW_W, WINDOW_H);
      break;

    default:
      return;
  }
  glutPostRedisplay ();
}

void
demo_view_special_func (demo_view_t *vu, int key, int x, int y)
{
  switch (key)
  {
    case GLUT_KEY_UP:
      demo_view_translate (vu, 0, -.1);
      break;
    case GLUT_KEY_DOWN:
      demo_view_translate (vu, 0, +.1);
      break;
    case GLUT_KEY_LEFT:
      demo_view_translate (vu, +.1, 0);
      break;
    case GLUT_KEY_RIGHT:
      demo_view_translate (vu, -.1, 0);
      break;

    default:
      return;
  }
  glutPostRedisplay ();
}

void
demo_view_mouse_func (demo_view_t *vu, int button, int state, int x, int y)
{
  if (state == GLUT_DOWN) {
    vu->buttons |= (1 << button);
    vu->click_handled = false;
  } else
    vu->buttons &= !(1 << button);
  vu->modifiers = glutGetModifiers ();

  switch (button)
  {
    case GLUT_RIGHT_BUTTON:
      switch (state) {
        case GLUT_DOWN:
	  if (vu->animate) {
	    demo_view_toggle_animation (vu);
	    vu->click_handled = true;
	  }
	  break;
        case GLUT_UP:
	  if (!vu->animate)
	    {
	      if (!vu->dragged && !vu->click_handled)
		demo_view_toggle_animation (vu);
	      else if (vu->dt) {
		double speed = hypot (vu->dx, vu->dy) / vu->dt;
		if (speed > 0.1)
		  demo_view_toggle_animation (vu);
	      }
	      vu->dx = vu->dy = vu->dt = 0;
	    }
	  break;
      }
      break;

#if defined(GLUT_WHEEL_UP)

    case GLUT_WHEEL_UP:
      demo_view_scale (vu, STEP);
      break;

    case GLUT_WHEEL_DOWN:
      demo_view_scale (vu, 1. / STEP);
      break;

#endif
  }

  vu->beginx = vu->lastx = x;
  vu->beginy = vu->lasty = y;
  vu->dragged = false;

  glutPostRedisplay ();
}

void
demo_view_motion_func (demo_view_t *vu, int x, int y)
{
  vu->dragged = true;

  int viewport[4];
  glGetIntegerv (GL_VIEWPORT, viewport);
  GLuint width  = viewport[2];
  GLuint height = viewport[3];

  if (vu->buttons & (1 << GLUT_LEFT_BUTTON))
  {
    if (vu->modifiers & GLUT_ACTIVE_CTRL) {
      /* adjust contrast/gamma */
      demo_view_scale_gamma_adjust (vu, 1 - ((y - vu->lasty) / height));
      demo_view_scale_contrast (vu, 1 + ((x - vu->lastx) / width));
    } else {
      /* translate */
      demo_view_translate (vu,
			   +2 * (x - vu->lastx) / width,
			   -2 * (y - vu->lasty) / height);
    }
  }

  if (vu->buttons & (1 << GLUT_RIGHT_BUTTON))
  {
    if (vu->modifiers & GLUT_ACTIVE_CTRL) {
      /* adjust perspective */
      demo_view_scale_perspective (vu, 1 - ((y - vu->lasty) / height) * 5);
    } else {
      /* rotate */
      trackball (vu->dquat,
		 (2.0*vu->lastx -         width) / width,
		 (       height - 2.0*vu->lasty) / height,
		 (        2.0*x -         width) / width,
		 (       height -         2.0*y) / height );

      vu->dx = x - vu->lastx;
      vu->dy = y - vu->lasty;
      vu->dt = current_time () - vu->lastt;

      add_quats (vu->dquat, vu->quat, vu->quat);
    }
  }

  if (vu->buttons & (1 << GLUT_MIDDLE_BUTTON))
  {
    /* scale */
    double factor = 1 - ((y - vu->lasty) / height) * 5;
    demo_view_scale (vu, factor);
    /* adjust translate so we scale centered at the drag-begin mouse position */
    demo_view_translate (vu,
			 +(2. * vu->beginx / width  - 1) * (1 - factor),
			 -(2. * vu->beginy / height - 1) * (1 - factor));
  }

  vu->lastx = x;
  vu->lasty = y;
  vu->lastt = current_time ();

  glutPostRedisplay ();
}

void
demo_view_print_help (demo_view_t *vu)
{
  printf ("Welcome to GLyphy demo\n");
}


void
demo_view_display (demo_view_t *vu, demo_buffer_t *buffer)
{
  int viewport[4];
  glGetIntegerv (GL_VIEWPORT, viewport);
  GLint width  = viewport[2];
  GLint height = viewport[3];

  glMatrixMode (GL_MODELVIEW);
  glLoadIdentity ();

  demo_view_apply_transform (vu);

  // Buffer best-fit
  glyphy_extents_t extents;
  demo_buffer_extents (buffer, &extents);
  double content_scale = .9 * std::min (width  / (extents.max_x - extents.min_x),
				        height / (extents.max_y - extents.min_y));
  glScaled (content_scale, content_scale, 1);
  // Center buffer
  glTranslated (-(extents.max_x + extents.min_x) / 2.,
		-(extents.max_y + extents.min_y) / 2., 0);

  GLfloat mat[16];
  glGetFloatv (GL_MODELVIEW_MATRIX, mat);
  glUniformMatrix4fv (glGetUniformLocation (vu->st->program, "u_matViewProjection"), 1, GL_FALSE, mat);

  glClearColor (1, 1, 1, 1);
  glClear (GL_COLOR_BUFFER_BIT);

  demo_buffer_draw (buffer);
  glutSwapBuffers ();
}

void
demo_view_setup (demo_view_t *vu)
{
  if (!vu->vsync)
    demo_view_toggle_vsync (vu);
  if (!vu->srgb)
    demo_view_toggle_srgb (vu);
  demo_state_setup (vu->st);
}