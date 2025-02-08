// Copyright (c) 2025 Charles Hood <chood@chood.net>
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.

#include <math.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

typedef enum {
	MOUSE_MODE_NONE,
	MOUSE_MODE_SELECT,
	MOUSE_MODE_PAN,
} MouseMode;

typedef struct {
	SDL_Window *window;
	int window_width;
	int window_height;
	GLint transformation_uniform;
	GLint selection_uniform;

	struct {
		float x;
		float y;
		float width;
		float height;
	} focus;

	MouseMode mouse_mode;
	int mouse_down_x;
	int mouse_down_y;
	int mouse_x;
	int mouse_y;
} App;

static void initialize(App *);
static GLuint create_program(char const *, char const *);
static GLuint create_shader(GLenum, char const *);
static void draw(App *);
static void get_transformation(App *, float *);
static void get_selection(App *, float const *, float *);
static void transform(float const *, float *);
static int cmp_int(void const *, void const *);
static void handle_event(App *, SDL_Event const *);
static void set_focus_from_selection(App *);
static void zoom(App *, float);
static void pan(App *, int, int);

static char const vert_shader_source[] = "\
#version 300 es\n\
\n\
uniform vec4 transformation;\n\
\n\
out vec2 frag_position;\n\
\n\
const vec2 vertices[] = vec2[](\n\
	vec2(-1., -1.),\n\
	vec2( 1., -1.),\n\
	vec2(-1.,  1.),\n\
	vec2( 1.,  1.)\n\
);\n\
\n\
const int indices[] = int[](0, 1, 2, 3, 2, 1);\n\
\n\
void\n\
main()\n\
{\n\
	vec2 p = vertices[indices[gl_VertexID]];\n\
	gl_Position = vec4(p, 0., 1.);\n\
	frag_position = transformation.zw * p + transformation.xy;\n\
}\n\
";

static char const frag_shader_source[] = "\
#version 300 es\n\
precision highp float;\n\
\n\
uniform vec4 selection;\n\
\n\
in vec2 frag_position;\n\
\n\
out vec4 out_color;\n\
\n\
void\n\
main()\n\
{\n\
	vec2 p = frag_position;\n\
\n\
	vec3 color = vec3(0., 0., .5);\n\
\n\
	vec2 z = p;\n\
	for (int i = 0; i < 256; ++i) {\n\
		z = vec2(z.x * z.x - z.y * z.y + p.x, 2. * z.x * z.y + p.y);\n\
	}\n\
\n\
	float limit = 3.;\n\
	if (-limit < z.x && z.x < limit && -limit < z.y && z.y < limit) {\n\
		color = vec3(1.);\n\
	}\n\
\n\
	if (selection.x <= p.x && p.x <= selection.z &&\n\
	    selection.y <= p.y && p.y <= selection.w) {\n\
		color = vec3(1.) - color;\n\
	}\n\
	out_color = vec4(color, 1.);\n\
}\n\
";

int
main(void)
{
	App app;
	initialize(&app);

	for (;;) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			handle_event(&app, &e);
		}

		draw(&app);

		if (!SDL_GL_SwapWindow(app.window)) {
			exit(EXIT_FAILURE);
		}

		if (!SDL_WaitEvent(&e)) {
			exit(EXIT_FAILURE);
		}
		handle_event(&app, &e);
	}
}

static void
initialize(App *app)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		exit(EXIT_FAILURE);
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
	    SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	char const *title = "Mandelbrot Set Visualizer";
	SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL |
	    SDL_WINDOW_MOUSE_CAPTURE;
	app->window = SDL_CreateWindow(title, 1280, 800, flags);
	if (app->window == NULL) {
		exit(EXIT_FAILURE);
	}
	if (!SDL_GetWindowSizeInPixels(app->window, &app->window_width,
	    &app->window_height)) {
		exit(EXIT_FAILURE);
	}

	SDL_GLContext context = SDL_GL_CreateContext(app->window);
	if (context == NULL) {
		exit(EXIT_FAILURE);
	}
	if (!SDL_GL_MakeCurrent(app->window, context)) {
		exit(EXIT_FAILURE);
	}

	GLuint program = create_program(vert_shader_source, frag_shader_source);
	if (program == 0) {
		exit(EXIT_FAILURE);
	}
	glUseProgram(program);

	app->transformation_uniform =
	    glGetUniformLocation(program, "transformation");
	app->selection_uniform = glGetUniformLocation(program, "selection");
	if (app->transformation_uniform == -1 || app->selection_uniform == -1) {
		exit(EXIT_FAILURE);
	}
	glViewport(0, 0, app->window_width, app->window_height);

	GLuint vertex_array;
	glGenVertexArrays(1, &vertex_array);
	glBindVertexArray(vertex_array);

	app->focus.x = 0.f;
	app->focus.y = 0.f;
	app->focus.width = 1.f;
	app->focus.height = 1.f;

	app->mouse_mode = MOUSE_MODE_NONE;
}

static GLuint
create_program(char const *vert_source, char const *frag_source)
{
	GLuint vert = create_shader(GL_VERTEX_SHADER, vert_source);
	GLuint frag = create_shader(GL_FRAGMENT_SHADER, frag_source);
	GLuint program = glCreateProgram();
	if (vert == 0 || frag == 0 || program == 0) {
		glDeleteShader(vert);
		glDeleteShader(frag);
		glDeleteProgram(program);
		return 0;
	}

	glAttachShader(program, vert);
	glAttachShader(program, frag);
	glLinkProgram(program);

	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		glDeleteProgram(program);
		return 0;
	}

	return program;
}

static GLuint
create_shader(GLenum type, char const *source)
{
	GLuint shader = glCreateShader(type);
	if (shader == 0) {
		return 0;
	}

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static void
draw(App *app)
{
	float t[4];
	get_transformation(app, t);

	float s[4];
	get_selection(app, t, s);

	glUniform4f(app->transformation_uniform, t[0], t[1], t[2], t[3]);
	glUniform4f(app->selection_uniform, s[0], s[1], s[2], s[3]);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void
get_transformation(App *app, float *t)
{
	t[0] = app->focus.x;
	t[1] = app->focus.y;

	float window_aspect_ratio =
	    (float)app->window_width / app->window_height;
	float focus_aspect_ratio =
	    app->focus.width / app->focus.height;

	if (window_aspect_ratio >= focus_aspect_ratio) {
		t[2] = app->focus.height * window_aspect_ratio;
		t[3] = app->focus.height;
	} else {
		t[2] = app->focus.width;
		t[3] = app->focus.width / window_aspect_ratio;
	}
}

static void
get_selection(App *app, float const *t, float *s)
{
	if (app->mouse_mode != MOUSE_MODE_SELECT) {
		memset(s, 0, 4 * sizeof(float));
		return;
	}

	int x[2] = {app->mouse_down_x, app->mouse_x};
	int y[2] = {
		app->window_height - app->mouse_down_y,
		app->window_height - app->mouse_y,
	};
	qsort(x, 2, sizeof(int), cmp_int);
	qsort(y, 2, sizeof(int), cmp_int);

	float w = app->window_width, h = app->window_height;
	s[0] = 2.f * x[0] / w - 1.f;
	s[1] = 2.f * y[0] / h - 1.f;
	s[2] = 2.f * x[1] / w - 1.f;
	s[3] = 2.f * y[1] / h - 1.f;

	transform(t, &s[0]);
	transform(t, &s[2]);
}

static int
cmp_int(void const *a, void const *b)
{
	return *(int *)a - *(int *)b;
}

static void
transform(float const *t, float *p)
{
	p[0] = t[2] * p[0] + t[0];
	p[1] = t[3] * p[1] + t[1];
}

static void
handle_event(App *app, SDL_Event const *e)
{
	switch (e->type) {
	case SDL_EVENT_QUIT:
		exit(EXIT_SUCCESS);
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		app->window_width = e->window.data1;
		app->window_height = e->window.data2;
		glViewport(0, 0, app->window_width, app->window_height);
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		zoom(app, powf(1.5f, -e->wheel.y));
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (app->mouse_mode != MOUSE_MODE_NONE) {
			break;
		}
		if (e->button.button == 1) {
			app->mouse_mode = MOUSE_MODE_SELECT;
			app->mouse_down_x = app->mouse_x = e->button.x;
			app->mouse_down_y = app->mouse_y = e->button.y;
			break;
		}
		if (e->button.button == 2) {
			app->mouse_mode = MOUSE_MODE_PAN;
			break;
		}
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (app->mouse_mode == MOUSE_MODE_SELECT) {
			set_focus_from_selection(app);
		}
		app->mouse_mode = MOUSE_MODE_NONE;
		break;
	case SDL_EVENT_MOUSE_MOTION:
		if (app->mouse_mode == MOUSE_MODE_PAN) {
			pan(app, e->motion.xrel, e->motion.yrel);
			break;
		}
		app->mouse_x = e->motion.x;
		app->mouse_y = e->motion.y;
		break;
	}
}

static void
set_focus_from_selection(App *app)
{
	float t[4], s[4];
	get_transformation(app, t);
	get_selection(app, t, s);

	if (s[0] == s[2] || s[1] == s[3]) {
		return;
	}

	app->focus.x = (s[0] + s[2]) * .5f;
	app->focus.y = (s[1] + s[3]) * .5f;
	app->focus.width = (s[2] - s[0]) * .5f;
	app->focus.height = (s[3] - s[1]) * .5f;
}

static void
zoom(App *app, float amount)
{
	float t[4];
	get_transformation(app, t);

	float x = (float)app->mouse_x / app->window_width;
	float y = 1.f - (float)app->mouse_y / app->window_height;
	float d[2] = {(2.f * x - 1.f) * t[2], (2.f * y - 1.f) * t[3]};

	app->focus.x += d[0] * (1.f - amount);
	app->focus.y += d[1] * (1.f - amount);
	app->focus.width *= amount;
	app->focus.height *= amount;
}

static void
pan(App *app, int x, int y)
{
	float t[4];
	get_transformation(app, t);

	float d[2] = {x, -y};
	d[0] = 2.f * t[2] * d[0] / app->window_width;
	d[1] = 2.f * t[3] * d[1] / app->window_height;

	app->focus.x -= d[0];
	app->focus.y -= d[1];
}
