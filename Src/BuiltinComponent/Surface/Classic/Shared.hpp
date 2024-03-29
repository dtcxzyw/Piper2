/*
   Copyright 2020-2021 Yingwei Zheng
   SPDX-License-Identifier: Apache-2.0

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#pragma once
#include "../../../Kernel/Protocol.hpp"

namespace Piper {
    struct DisneyPrincipledBRDFData final {
        Dimensionless<float> metallic;
        Dimensionless<float> specularTint;
        Dimensionless<float> roughness;
        Dimensionless<float> anisotropic;
        Dimensionless<float> sheen;
        Dimensionless<float> sheenTint;
        Dimensionless<float> clearcoat;
        Dimensionless<float> clearcoatGloss;
        Dimensionless<float> specTrans;
        Dimensionless<float> IOR;
        Spectrum<Dimensionless<float>> scatterDist;
        Spectrum<Dimensionless<float>> baseColor;
    };
    struct DisneyThinSurfaceData final {
        Dimensionless<float> metallic;
        Dimensionless<float> diffTrans;
        Dimensionless<float> specularTint;
        Dimensionless<float> roughness;
        Dimensionless<float> anisotropic;
        Dimensionless<float> sheen;
        Dimensionless<float> sheenTint;
        Dimensionless<float> clearcoat;
        Dimensionless<float> clearcoatGloss;
        Dimensionless<float> specTrans;
        Dimensionless<float> IOR;
        Dimensionless<float> flatness;
    };
    // TODO:BumpedMaterial
    struct MatteData final {
        Call<TextureSampleFunc> diffuseTexture;
        Call<TextureSampleFunc> roughnessTexture;
    };

    struct PlasticData final {
        Call<TextureSampleFunc> diffuseTexture;
        Call<TextureSampleFunc> specularTexture;
        Call<TextureSampleFunc> roughnessTexture;
    };

    struct MirrorData final {
        Call<TextureSampleFunc> reflectionTexture;
    };

    struct GlassData final {
        Call<TextureSampleFunc> reflection;
        Call<TextureSampleFunc> transmission;
        Call<TextureSampleFunc> roughnessX;
        Call<TextureSampleFunc> roughnessY;
    };

    struct SubstrateData final {
        Call<TextureSampleFunc> diffuse;
        Call<TextureSampleFunc> specular;
        Call<TextureSampleFunc> roughnessX;
        Call<TextureSampleFunc> roughnessY;
    };
}  // namespace Piper
