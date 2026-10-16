#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "rtos.h"

#include "schneider_model.hpp"

namespace omniboat {

Schneider::Schneider() :
    phi(0),
    gyro(),
    t_jacobianmatrix(),
    x_d(),
    q(),
    x(),
    adcIn1(A4),
    adcIn2(A5),
    volume(A6),
    mpu(D4, D5),
    servo_1(PB_4),
    servo_2(PA_11),
    fet_1(PA_9),
    fet_2(PA_10),
    led1(PA_1),
    led2(PA_3),
    led3(PA_4),
    pc(USBTX, USBRX) {
    led(1);
    led(2);
    led(3);
    printf("start up\n");

    servo_1.period_ms(20);
    servo_2.period_ms(20);
}

Schneider::~Schneider() {
    led(1);
}

void Schneider::init() {
    using std::fill;
    phi = 0;
    for (auto& row : t_jacobianmatrix) {
        fill(row.begin(), row.end(), 0);
    }
    fill(q.begin(), q.end(), 0.01F);
    fill(x.begin(), x.end(), 0);
    cal_tjacob();
    const bool whoami = mpu.testConnection();
    if (whoami) {
        printf("WHOAMI succeeded\n");
    } else {
        printf("WHOAMI failed\n");
    }
}

void Schneider::one_step() {
    using std::abs;
    led(3);

    // ジャイロセンサの値を読み取る
    mpu.getGyro(gyro.data());

    // ジョイコンの値を読み取る
    joy_read(adcIn1.read(), adcIn2.read(), 0.0);

    volume_ = volume.read();

    q[0] = 0;
    q[1] = 0;

    const bool joyEffective
        = abs(x_d[0]) > joyEffectiveRangeMin || abs(x_d[1]) > joyEffectiveRangeMin;
    const bool volumeEffective
        = volumeEffectiveRange[0] < volume_ && volume_ < volumeEffectiveRange[1];

    if (joyEffective) {
        cal_q();
        set_q();
    } else if (volumeEffective) {
        rotate();
        phi = 0;
    } else {
        fet_1 = 0;
        fet_2 = 0;
        phi = 0;
    }
    debug();
    led(3);
}

void Schneider::debug() {
    // printf("phi=%f\n",phi);
    // printf("ave=%f\n",ave_[0]/ave_[1]);
    // printf("motor=%f,%f\n",q[0],q[1]);
    // printf("servo=%f,%f\n",q[2],q[3]);
    // printf("volume=%f\n",volume.read());
    // printf("gyro[2]=%f\n",gyro[2]);
    // printf("\n");
}

void Schneider::joy_read(float joy_x, float joy_y, int rotate) {
    x_d[0] = (joy_x - 0.5F) * 2;
    x_d[1] = (joy_y - 0.5F) * 2;
    // x_d[2] = rotate;
    x_d[2] = 0;
}

inline void Schneider::cal_tjacob() {
    using std::cos;
    using std::sin;
    t_jacobianmatrix[0][0] = cos(q[2] + phi);
    t_jacobianmatrix[0][1] = sin(q[2] + phi);
    t_jacobianmatrix[0][2] = (a + sin(q[2])) / I;
    t_jacobianmatrix[1][0] = cos(q[3] + phi);
    t_jacobianmatrix[1][1] = sin(q[3] + phi);
    t_jacobianmatrix[1][2] = (-a - sin(q[3])) / I;
    t_jacobianmatrix[2][0] = -q[0] * sin(q[2] + phi);
    t_jacobianmatrix[2][1] = q[0] * cos(q[2] + phi);
    t_jacobianmatrix[2][2] = q[0] * cos(q[2]) / I;
    t_jacobianmatrix[3][0] = -q[1] * sin(q[3] + phi);
    t_jacobianmatrix[3][1] = q[1] * cos(q[3] + phi);
    t_jacobianmatrix[3][2] = -q[1] * cos(q[3]) / I;
}

void Schneider::cal_q() {
    using std::pow;
    // 初期値
    const float coef = (x_d[0] >= 0 && x_d[1] >= 0)  ? 1
                       : (x_d[0] >= 0 && x_d[1] < 0) ? -1
                       : (x_d[0] < 0 && x_d[1] >= 0) ? 3
                                                     : 5;
    for (int i = 2; i < 4; ++i) {
        q[i] = coef * schneider_PI / 4 - phi;
    }

    led(2);
    for (int i = 0; i < trial_num; i++) {
        state_equation();

        double diff = pow(x[0] - x_d[0], 2) + pow(x[1] - x_d[1], 2) + pow(x[2] - x_d[2], 2);
        if (diff < diffEffectiveRangeMin) {
            break;
        }

        cal_tjacob();
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 3; k++) {
                q[j] -= e * t_jacobianmatrix[j][k] * (x[k] - x_d[k]);
            }
            if (j == 0 || j == 1) {
                q[j] -= pow(2 * q[j] - 1, 7);
            }
        }
    }
    led(2);
}

inline void Schneider::state_equation() {
    using std::cos;
    using std::sin;
    x[0] = q[0] * cos(q[2] + phi) + q[1] * cos(q[3] + phi);
    x[1] = q[0] * sin(q[2] + phi) + q[1] * sin(q[3] + phi);
    x[2] = (a * (q[0] - q[1]) + q[0] * sin(q[2]) - q[1] * sin(q[3])) / I;
}

void Schneider::set_q() {
    using std::abs;
    if (abs(this->q[0] <= joyEffectiveRangeMin)) {
        this->q[0] = 0;
    }
    if (abs(this->q[1] <= joyEffectiveRangeMin)) {
        this->q[1] = 0;
    }
    this->fet_1 = this->q[0];
    this->fet_2 = this->q[1];

    while (this->q[2] >= schneider_PI) {
        this->q[2] -= 2 * schneider_PI;
    }
    while (this->q[3] >= schneider_PI) {
        this->q[3] -= 2 * schneider_PI;
    }
    while (this->q[2] < -schneider_PI) {
        this->q[2] += 2 * schneider_PI;
    }
    while (this->q[3] < -schneider_PI) {
        this->q[3] += 2 * schneider_PI;
    }

    if (0 < this->q[2] && this->q[2] < schneider_PI) {
        const int width
            = static_cast<int>(500 + 1900 / schneider_PI * this->q[2] - 2200 * this->gyro[2]);
        this->servo_1.pulsewidth_us(width);
    }
    if (0 < this->q[3] && this->q[3] < schneider_PI) {
        const int width
            = static_cast<int>(500 + 1900 / schneider_PI * this->q[3] + 2200 * this->gyro[2]);
        this->servo_2.pulsewidth_us(width);
    }
}

void Schneider::rotate() {
    fet_1 = 0.5F;
    fet_2 = 0.5F;
    // ifとelseで内容が同じだといわれたがそんなことない
    // NOLINTBEGIN(bugprone-branch-clone)
    if (volume_ < volumeThreshold) {
        this->servo_1.pulsewidth_us(minorRotatePulsewidthUs);
        this->servo_2.pulsewidth_us(majorRotatePulsewidthUs);
    } else {
        this->servo_2.pulsewidth_us(minorRotatePulsewidthUs);
        this->servo_1.pulsewidth_us(majorRotatePulsewidthUs);
    }
    // NOLINTEND(bugprone-branch-clone)
}

void Schneider::led(int num) {
    // 3つの連続した明示的なブランチがあるといわれたがそんなことない
    // NOLINTBEGIN(bugprone-branch-clone)
    switch (num) {
    case 1: this->led1 = !this->led1; break;
    case 2: this->led2 = !this->led2; break;
    case 3: this->led3 = !this->led3; break;
    default: break;
    }
    // NOLINTEND(bugprone-branch-clone)
}

}  // namespace omniboat
