# pj_scene3D — third-party derived GLSL

Shader snippets in this module derived from external sources. Each file
carrying derived shader code opens with a comment block naming the source;
full license texts live here.

## AgX tonemapper (`scene_view_widget.cpp` composite present shader; `widgets/rhi/shaders/present.frag`)

Adapted from three.js `tonemapping_pars_fragment.glsl.js` (AgX implementation,
itself derived from Filament's AgX by Benoit Mayaux and Troy Sobotka's
reference). License: **MIT**.

> Copyright © 2010-2024 three.js authors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
> FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.

## ACES filmic approximation (`scene_view_widget.cpp`, composite present shader)

Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" (2016). Released by the
author as **CC0 / public domain**.

## Khronos PBR Neutral tonemapper (`scene_view_widget.cpp` composite present shader; `widgets/rhi/shaders/present.frag`)

The `PBRNeutral` tone-mapping curve is the **Khronos PBR Neutral** tone mapper
from the glTF Sample Viewer, adopted via three.js `NeutralToneMapping`
(`tonemapping_pars_fragment.glsl.js`). License: **Apache-2.0** (Khronos) /
**MIT** (three.js port). The three.js MIT text is reproduced under the AgX
section above; the Apache-2.0 grant is the standard Khronos license.

## sRGB OETF (`scene_view_widget.cpp`, composite present shader)

The piecewise sRGB encode function per **IEC 61966-2-1** (a standard formula,
not copyrightable expression; listed for provenance).

## Analytic environment BRDF (`passes/mesh_render_pass.cpp`, mesh fragment shader)

`envBRDFApprox` is Brian Karis' analytic fit to the split-sum environment BRDF
("Physically Based Shading on Mobile", Epic Games, 2014;
https://www.unrealengine.com/blog/physically-based-shading-on-mobile). Released
by the author for free use; a short polynomial, listed for provenance. The
surrounding analytic-IBL ambient (procedural ground→sky `envRadiance`,
roughness-blurred reflection prefilter) is original PJ4 code.

## SSAO (`passes/ssao_pass.cpp`)

Hemisphere-kernel SSAO and the 4×4 tile blur adapted from **LearnOpenGL**
("SSAO", Joey de Vries), licensed **CC BY 4.0**
(https://creativecommons.org/licenses/by/4.0/); source:
https://learnopengl.com/Advanced-Lighting/SSAO. Modified: view-space position
reconstructs through the inverse projection matrix (supports orthographic
cameras) and the rotation noise is an in-shader 4×4-tiled hash.

## Depth→normal reconstruction (`passes/ssao_pass.cpp`)

The closer-derivative 5-tap normal reconstruction adapted from **Ben Golus**,
"Normals from depth" (MIT).

## EDL — eye-dome lighting (`passes/edl_pass.cpp`)

Shade-factor algorithm derived from **Potree**'s `EDLRenderer`/`edl.fs`
(https://github.com/potree/potree), licensed **BSD-2-Clause**, © 2011-2020
Markus Schütz. The EDL technique originates with **Christian Boucheny**
(thesis, 2009) as implemented in **CloudCompare**. Modified: eye-space depth
reconstructs through the inverse projection matrix (supports orthographic
cameras); background neighbours are treated as infinitely far.

> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.
