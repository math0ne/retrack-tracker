// All-pass diffusion network - ported from Mutable Instruments Clouds.
// Copyright 2014 Emilie Gillet (MIT License)
//
// Simplified implementation without the FxEngine template.
#pragma once

#include <cstdint>
#include <cstring>

namespace clouds {

// Simple all-pass delay line
class AllPassDelay {
public:
    void Init(float* buffer, int32_t size) {
        buffer_ = buffer;
        size_ = size;
        write_ptr_ = 0;
        std::memset(buffer_, 0, size * sizeof(float));
    }

    inline float Process(float input, float coefficient) {
        float read = buffer_[write_ptr_];
        float write = input + coefficient * read;
        buffer_[write_ptr_] = write;
        ++write_ptr_;
        if (write_ptr_ >= size_) write_ptr_ = 0;
        return read - coefficient * write;
    }

private:
    float* buffer_;
    int32_t size_;
    int32_t write_ptr_;
};

// 4-stage all-pass diffuser (one per channel)
class Diffuser {
public:
    Diffuser() : amount_(0.0f) { }
    ~Diffuser() { }

    // Needs 2048 floats of workspace
    void Init(float* buffer) {
        // Left channel delays: 126, 180, 269, 444
        ap_l_[0].Init(&buffer[0], 126);
        ap_l_[1].Init(&buffer[126], 180);
        ap_l_[2].Init(&buffer[306], 269);
        ap_l_[3].Init(&buffer[575], 444);

        // Right channel delays: 151, 205, 245, 405
        ap_r_[0].Init(&buffer[1019], 151);
        ap_r_[1].Init(&buffer[1170], 205);
        ap_r_[2].Init(&buffer[1375], 245);
        ap_r_[3].Init(&buffer[1620], 405);
    }

    void Process(float* in_out_l, float* in_out_r, size_t size) {
        const float kap = 0.625f;
        const float amount = amount_;

        while (size--) {
            float wet_l = *in_out_l;
            wet_l = ap_l_[0].Process(wet_l, kap);
            wet_l = ap_l_[1].Process(wet_l, kap);
            wet_l = ap_l_[2].Process(wet_l, kap);
            wet_l = ap_l_[3].Process(wet_l, kap);
            *in_out_l = *in_out_l + amount * (wet_l - *in_out_l);
            ++in_out_l;

            float wet_r = *in_out_r;
            wet_r = ap_r_[0].Process(wet_r, kap);
            wet_r = ap_r_[1].Process(wet_r, kap);
            wet_r = ap_r_[2].Process(wet_r, kap);
            wet_r = ap_r_[3].Process(wet_r, kap);
            *in_out_r = *in_out_r + amount * (wet_r - *in_out_r);
            ++in_out_r;
        }
    }

    void set_amount(float amount) {
        amount_ = amount;
    }

private:
    AllPassDelay ap_l_[4];
    AllPassDelay ap_r_[4];
    float amount_;
};

}  // namespace clouds
