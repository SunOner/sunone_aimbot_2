#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace aim
{
struct AimKalmanSettings
{
    bool enabled = true;
    double process_noise_position = 40.0;
    double process_noise_velocity = 1800.0;
    double measurement_noise = 35.0;
    double velocity_damping = 0.08;
    double max_velocity = 20000.0;
    int warmup_frames = 2;
};

struct AimKalmanTelemetry
{
    bool initialized = false;
    bool enabled = true;
    double dt = 0.0;
    int warmup_remaining = 0;
    bool prediction_ready = false;

    double measurement_x = 0.0;
    double measurement_y = 0.0;
    double estimate_x = 0.0;
    double estimate_y = 0.0;
    double predicted_x = 0.0;
    double predicted_y = 0.0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
    double innovation_x = 0.0;
    double innovation_y = 0.0;
};

class AimKalman2D
{
public:
    AimKalman2D() = default;

    void setSettings(const AimKalmanSettings& settings)
    {
        settings_ = clampSettings(settings);
        warmupRemaining_ = std::clamp(warmupRemaining_, 0, settings_.warmup_frames);
    }

    const AimKalmanSettings& settings() const
    {
        return settings_;
    }

    void reset()
    {
        xAxis_ = AxisState();
        yAxis_ = AxisState();
        initialized_ = false;
        hasLastMeasurement_ = false;
        warmupRemaining_ = settings_.warmup_frames;
    }

    bool initialized() const
    {
        return initialized_;
    }

    std::pair<double, double> position() const
    {
        return { xAxis_.position, yAxis_.position };
    }

    std::pair<double, double> velocity() const
    {
        return { xAxis_.velocity, yAxis_.velocity };
    }

    std::pair<double, double> predict(double lookaheadSec) const
    {
        if (!initialized_)
            return {};

        const double lookahead = std::clamp(lookaheadSec, 0.0, 1.5);
        if (lookahead <= 0.0 || warmupRemaining_ > 0)
            return position();

        return {
            predictAxis(xAxis_, settings_.velocity_damping, lookahead),
            predictAxis(yAxis_, settings_.velocity_damping, lookahead)
        };
    }

    AimKalmanTelemetry update(double measurementX, double measurementY, double dt, double lookaheadSec)
    {
        AimKalmanTelemetry telemetry;
        telemetry.enabled = settings_.enabled;
        telemetry.measurement_x = measurementX;
        telemetry.measurement_y = measurementY;

        const double clampedDt = std::clamp(dt, 1e-4, 0.25);
        const double lookahead = std::clamp(lookaheadSec, 0.0, 1.5);
        telemetry.dt = clampedDt;

        if (!initialized_)
        {
            initialize(measurementX, measurementY);
            telemetry = buildTelemetry(measurementX, measurementY, 0.0, 0.0, lookahead);
            telemetry.dt = clampedDt;
            telemetry.innovation_x = 0.0;
            telemetry.innovation_y = 0.0;
            return telemetry;
        }

        if (!settings_.enabled)
        {
            const double prevX = xAxis_.position;
            const double prevY = yAxis_.position;

            const double velocityX = clampAbs((measurementX - lastMeasurementX_) / clampedDt, settings_.max_velocity);
            const double velocityY = clampAbs((measurementY - lastMeasurementY_) / clampedDt, settings_.max_velocity);

            xAxis_.position = measurementX;
            yAxis_.position = measurementY;
            xAxis_.velocity = velocityX;
            yAxis_.velocity = velocityY;

            lastMeasurementX_ = measurementX;
            lastMeasurementY_ = measurementY;
            hasLastMeasurement_ = true;

            telemetry = buildTelemetry(measurementX, measurementY, measurementX - prevX, measurementY - prevY, lookahead);
            telemetry.dt = clampedDt;
            return telemetry;
        }

        const double innovationX = updateAxis(xAxis_, measurementX, clampedDt);
        const double innovationY = updateAxis(yAxis_, measurementY, clampedDt);

        lastMeasurementX_ = measurementX;
        lastMeasurementY_ = measurementY;
        hasLastMeasurement_ = true;

        telemetry = buildTelemetry(measurementX, measurementY, innovationX, innovationY, lookahead);
        telemetry.dt = clampedDt;
        return telemetry;
    }

private:
    struct AxisState
    {
        double position = 0.0;
        double velocity = 0.0;
        double p00 = 150.0;
        double p01 = 0.0;
        double p10 = 0.0;
        double p11 = 700.0;
    };

    static AimKalmanSettings clampSettings(const AimKalmanSettings& in)
    {
        AimKalmanSettings out = in;
        out.process_noise_position = std::clamp(out.process_noise_position, 1e-4, 5000.0);
        out.process_noise_velocity = std::clamp(out.process_noise_velocity, 1e-4, 50000.0);
        out.measurement_noise = std::clamp(out.measurement_noise, 1e-4, 5000.0);
        out.velocity_damping = std::clamp(out.velocity_damping, 0.0, 3.0);
        out.max_velocity = std::clamp(out.max_velocity, 100.0, 60000.0);
        out.warmup_frames = std::clamp(out.warmup_frames, 0, 20);
        return out;
    }

    static double clampAbs(double value, double maxAbs)
    {
        return std::clamp(value, -maxAbs, maxAbs);
    }

    static double predictAxis(const AxisState& axis, double damping, double lookahead)
    {
        if (lookahead <= 0.0)
            return axis.position;

        if (damping <= 1e-6)
            return axis.position + axis.velocity * lookahead;

        const double decay = std::exp(-damping * lookahead);
        return axis.position + axis.velocity * (1.0 - decay) / damping;
    }

    void initialize(double measurementX, double measurementY)
    {
        xAxis_.position = measurementX;
        yAxis_.position = measurementY;
        xAxis_.velocity = 0.0;
        yAxis_.velocity = 0.0;
        xAxis_.p00 = settings_.measurement_noise;
        yAxis_.p00 = settings_.measurement_noise;
        xAxis_.p11 = settings_.process_noise_velocity;
        yAxis_.p11 = settings_.process_noise_velocity;
        xAxis_.p01 = xAxis_.p10 = 0.0;
        yAxis_.p01 = yAxis_.p10 = 0.0;
        warmupRemaining_ = settings_.warmup_frames;
        initialized_ = true;
        hasLastMeasurement_ = true;
        lastMeasurementX_ = measurementX;
        lastMeasurementY_ = measurementY;
    }

    double updateAxis(AxisState& axis, double measurement, double dt)
    {
        const double dampingFactor = std::exp(-settings_.velocity_damping * dt);
        axis.position += axis.velocity * dt;
        axis.velocity *= dampingFactor;

        const double oldP00 = axis.p00;
        const double oldP01 = axis.p01;
        const double oldP10 = axis.p10;
        const double oldP11 = axis.p11;

        axis.p00 = oldP00 + dt * (oldP01 + oldP10 + dt * oldP11) + settings_.process_noise_position * dt;
        axis.p01 = oldP01 + dt * oldP11;
        axis.p10 = oldP10 + dt * oldP11;
        axis.p11 = oldP11 + settings_.process_noise_velocity * dt;

        const double innovation = measurement - axis.position;
        const double s = std::max(1e-9, axis.p00 + settings_.measurement_noise);
        const double k0 = axis.p00 / s;
        const double k1 = axis.p10 / s;

        axis.position += k0 * innovation;
        axis.velocity += k1 * innovation;
        axis.velocity = clampAbs(axis.velocity, settings_.max_velocity);

        const double p00 = axis.p00;
        const double p01 = axis.p01;
        const double p10 = axis.p10;
        const double p11 = axis.p11;

        axis.p00 = std::max(1e-9, (1.0 - k0) * p00);
        axis.p01 = (1.0 - k0) * p01;
        axis.p10 = p10 - k1 * p00;
        axis.p11 = std::max(1e-9, p11 - k1 * p01);

        return innovation;
    }

    AimKalmanTelemetry buildTelemetry(
        double measurementX,
        double measurementY,
        double innovationX,
        double innovationY,
        double lookaheadSec)
    {
        AimKalmanTelemetry telemetry;
        telemetry.initialized = initialized_;
        telemetry.enabled = settings_.enabled;
        telemetry.warmup_remaining = warmupRemaining_;
        telemetry.measurement_x = measurementX;
        telemetry.measurement_y = measurementY;
        telemetry.estimate_x = xAxis_.position;
        telemetry.estimate_y = yAxis_.position;
        telemetry.velocity_x = xAxis_.velocity;
        telemetry.velocity_y = yAxis_.velocity;
        telemetry.innovation_x = innovationX;
        telemetry.innovation_y = innovationY;

        if (warmupRemaining_ > 0)
        {
            --warmupRemaining_;
            telemetry.predicted_x = xAxis_.position;
            telemetry.predicted_y = yAxis_.position;
            telemetry.warmup_remaining = warmupRemaining_;
            return telemetry;
        }

        telemetry.prediction_ready = true;
        auto future = predict(lookaheadSec);
        telemetry.predicted_x = future.first;
        telemetry.predicted_y = future.second;
        return telemetry;
    }

private:
    AimKalmanSettings settings_{};
    AxisState xAxis_{};
    AxisState yAxis_{};
    bool initialized_ = false;
    bool hasLastMeasurement_ = false;
    int warmupRemaining_ = 0;
    double lastMeasurementX_ = 0.0;
    double lastMeasurementY_ = 0.0;
};
} // namespace aim
