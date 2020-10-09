#include <cstdint>

extern "C" void saxpy(uint32_t idx, float alpha, uint64_t pX, uint64_t pY, uint64_t pZ) {
    const float* X = reinterpret_cast<const float*>(pX);
    const float* Y = reinterpret_cast<const float*>(pY);
    float* Z = reinterpret_cast<float*>(pZ);
    Z[idx] = alpha * X[idx] + Y[idx];
}
