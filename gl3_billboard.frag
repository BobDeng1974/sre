/*

Copyright (c) 2014 Harm Hanemaaijer <fgenfb@yahoo.com>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

*/

// Billboard fragment shader. The model objects are billboards in
// world space. Used for both billboard particle systems and single billboards.
//
// It has been written to be compatible with both OpenGL 2.0+ and OpenGL ES 2.0.

#ifdef GL_ES
precision highp float;
#endif
uniform vec3 base_color_in; // Actually emission color.
uniform bool use_emission_map_in;
uniform sampler2D texture_in;
varying vec2 texcoord_var;

void main() {
	vec3 c;
        vec4 emission_map_color;
	if (use_emission_map_in) {
		emission_map_color = texture2D(texture_in, texcoord_var);
		c = base_color_in * emission_map_color.rgb;
	}
        else
		c = base_color_in;
	gl_FragColor = vec4(c, emission_map_color.a);
}

