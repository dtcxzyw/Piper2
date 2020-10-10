#include <cstdint>

struct Payload {
    uint64_t pX;
    uint64_t pY;
    uint64_t pZ;
    float alpha;
};

extern "C" void saxpy(const uint32_t idx, const Payload* payload) {
    const float* X = reinterpret_cast<const float*>(payload->pX);
    const float* Y = reinterpret_cast<const float*>(payload->pY);
    float* Z = reinterpret_cast<float*>(payload->pZ);
    Z[idx] += payload->alpha * X[idx] + Y[idx];
}
