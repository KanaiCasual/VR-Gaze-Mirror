// VR Gaze Mirror 2.0 - core: the grid map behind the thorough VRChat calibration, and the sums that make one.
//
// The quick calibration fits a curve per axis from twenty looks. The thorough one has the user follow a moving target
// and records everything that arrives: a few thousand pairs of "the target was here" and "the eyes said this". From
// those a GRID is fitted: nodes spread over the eye values, a tangent pair (right, up) at each, bilinear in between
// and straight on beyond the edges. Nothing here touches SteamVR or the settings file, so the offline test runs the
// whole fit on made-up eyes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace gaze_mirror::grid {

    constexpr float Pi = 3.14159265f;

    // The map, as the core keeps it (vrchat_map_grid in the settings file).
    struct Grid {
        int cols = 0, rows = 0;           // Nodes across (eye value x) and down (eye value y).
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0; // The eye values at the first and last node of each axis.
        std::vector<float> tx, ty;        // rows * cols tangents, row-major (y outer).
        bool valid() const {
            return cols >= 2 && rows >= 2 && x1 > x0 && y1 > y0 && tx.size() == size_t(cols) * rows && ty.size() == tx.size();
        }
    };

    // Where an eye-value pair lands: bilinear inside the grid, continued straight (at most one cell) beyond its edges.
    inline void Lookup(const Grid& g, float vx, float vy, float& tx, float& ty) {
        const float fx = std::clamp((vx - g.x0) / (g.x1 - g.x0) * float(g.cols - 1), -1.f, float(g.cols));
        const float fy = std::clamp((vy - g.y0) / (g.y1 - g.y0) * float(g.rows - 1), -1.f, float(g.rows));
        const int cx = std::clamp(int(std::floor(fx)), 0, g.cols - 2), cy = std::clamp(int(std::floor(fy)), 0, g.rows - 2);
        const float u = fx - float(cx), v = fy - float(cy); // Outside 0..1 beyond the edges: the cell's plane runs on.
        const auto at = [&](const std::vector<float>& t, int x, int y) { return t[size_t(y) * g.cols + x]; };
        const auto bilinear = [&](const std::vector<float>& t) {
            return (1 - u) * (1 - v) * at(t, cx, cy) + u * (1 - v) * at(t, cx + 1, cy) + (1 - u) * v * at(t, cx, cy + 1) + u * v * at(t, cx + 1, cy + 1);
        };
        tx = bilinear(g.tx);
        ty = bilinear(g.ty);
    }

    // ---- the moving target's path -------------------------------------------------------------------------------------

    // One moment of the target: where it is (tangents, right and up positive) and whether it is resting on a node.
    struct Waypoint {
        float timeMs;
        float tanX, tanY;
        bool paused;
    };

    // The path over a 5 x 5 grid of directions: yaw -left..right, pitch -down..up (degrees), row by row and back
    // (top row rightwards, the next leftwards, ...), then column by column the same way. It rests `pauseMs` on every
    // node and moves at `speedDegPerS` between them; the eyes get `settleMs` at the centre first. Sampled every
    // `stepMs`; the path ends with the target back at the centre.
    inline std::vector<Waypoint> BuildPath(float leftDeg, float rightDeg, float upDeg, float downDeg, float speedDegPerS, float pauseMs,
                                           float settleMs, float stepMs) {
        struct Node {
            float yaw, pitch;
        };
        const float yaws[5] = {-leftDeg, -leftDeg * 0.5f, 0.f, rightDeg * 0.5f, rightDeg};
        const float pitches[5] = {upDeg, upDeg * 0.5f, 0.f, -downDeg * 0.5f, -downDeg};
        std::vector<Node> nodes;
        nodes.push_back({0.f, 0.f});
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 5; c++) nodes.push_back({yaws[r % 2 == 0 ? c : 4 - c], pitches[r]});
        }
        for (int c = 0; c < 5; c++) {
            for (int r = 0; r < 5; r++) nodes.push_back({yaws[c], pitches[c % 2 == 0 ? r : 4 - r]});
        }
        nodes.push_back({0.f, 0.f});

        std::vector<Waypoint> path;
        float t = 0.f;
        const auto rest = [&](const Node& n, float ms, bool paused) {
            const float end = t + ms;
            for (; t < end; t += stepMs) path.push_back({t, std::tan(n.yaw * Pi / 180.f), std::tan(n.pitch * Pi / 180.f), paused});
        };
        rest(nodes[0], settleMs, false);
        for (size_t i = 1; i < nodes.size(); i++) {
            const Node& a = nodes[i - 1];
            const Node& b = nodes[i];
            const float length = std::sqrt((b.yaw - a.yaw) * (b.yaw - a.yaw) + (b.pitch - a.pitch) * (b.pitch - a.pitch));
            const float travelMs = length / speedDegPerS * 1000.f;
            const float start = t;
            for (; t < start + travelMs; t += stepMs) {
                const float f = (t - start) / travelMs;
                const float yaw = a.yaw + (b.yaw - a.yaw) * f, pitch = a.pitch + (b.pitch - a.pitch) * f;
                path.push_back({t, std::tan(yaw * Pi / 180.f), std::tan(pitch * Pi / 180.f), false});
            }
            rest(b, pauseMs, true);
        }
        return path;
    }

    // ---- the fit --------------------------------------------------------------------------------------------------------

    // One moment of the recording: the target of that moment and the eye values that were current.
    struct Record {
        float timeMs;
        float tanX, tanY; // The target.
        bool paused;
        float valueX, valueY;
        bool fresh; // New eye values arrived since the record before (VRChat sends fewer than we record).
        bool open;  // Both eyes open enough to count (and a moment past a blink).
    };

    struct Sample {
        float vx, vy, tx, ty, weight;
    };

    // Gaussian elimination with partial pivoting: A x = b for two right-hand sides, in place. False when singular.
    inline bool Solve(std::vector<double>& A, std::vector<double>& b1, std::vector<double>& b2, int n) {
        for (int col = 0; col < n; col++) {
            int pivot = col;
            for (int r = col + 1; r < n; r++) {
                if (std::fabs(A[size_t(r) * n + col]) > std::fabs(A[size_t(pivot) * n + col])) pivot = r;
            }
            if (std::fabs(A[size_t(pivot) * n + col]) < 1e-12) return false;
            if (pivot != col) {
                for (int c = 0; c < n; c++) std::swap(A[size_t(col) * n + c], A[size_t(pivot) * n + c]);
                std::swap(b1[col], b1[pivot]);
                std::swap(b2[col], b2[pivot]);
            }
            for (int r = col + 1; r < n; r++) {
                const double f = A[size_t(r) * n + col] / A[size_t(col) * n + col];
                if (f == 0.0) continue;
                for (int c = col; c < n; c++) A[size_t(r) * n + c] -= f * A[size_t(col) * n + c];
                b1[r] -= f * b1[col];
                b2[r] -= f * b2[col];
            }
        }
        for (int r = n - 1; r >= 0; r--) {
            for (int c = r + 1; c < n; c++) {
                b1[r] -= A[size_t(r) * n + c] * b1[c];
                b2[r] -= A[size_t(r) * n + c] * b2[c];
            }
            b1[r] /= A[size_t(r) * n + r];
            b2[r] /= A[size_t(r) * n + r];
        }
        return true;
    }

    // Least squares over the samples, with a light pull towards straight continuation between neighbouring nodes so
    // that nodes nobody looked near still get a sensible value. The grid covers the 1st..99th percentile of the values.
    inline bool FitGrid(const std::vector<Sample>& samples, int cols, int rows, Grid& out) {
        if (samples.size() < 50) return false;
        std::vector<float> xs, ys;
        xs.reserve(samples.size());
        ys.reserve(samples.size());
        for (const Sample& s : samples) {
            xs.push_back(s.vx);
            ys.push_back(s.vy);
        }
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        const auto percentile = [](const std::vector<float>& v, float p) { return v[std::min(v.size() - 1, size_t(p * float(v.size())))]; };
        out.cols = cols;
        out.rows = rows;
        out.x0 = percentile(xs, 0.01f);
        out.x1 = percentile(xs, 0.99f);
        out.y0 = percentile(ys, 0.01f);
        out.y1 = percentile(ys, 0.99f);
        const float padX = 0.02f * (out.x1 - out.x0), padY = 0.02f * (out.y1 - out.y0);
        out.x0 -= padX;
        out.x1 += padX;
        out.y0 -= padY;
        out.y1 += padY;
        if (out.x1 - out.x0 < 1e-4f || out.y1 - out.y0 < 1e-4f) return false;

        const int n = cols * rows;
        std::vector<double> A(size_t(n) * n, 0.0), bx(n, 0.0), by(n, 0.0);
        double totalWeight = 0.0;
        for (const Sample& s : samples) {
            const float fx = std::clamp((s.vx - out.x0) / (out.x1 - out.x0) * float(cols - 1), 0.f, float(cols - 1));
            const float fy = std::clamp((s.vy - out.y0) / (out.y1 - out.y0) * float(rows - 1), 0.f, float(rows - 1));
            const int cx = std::min(int(fx), cols - 2), cy = std::min(int(fy), rows - 2);
            const double u = fx - float(cx), v = fy - float(cy);
            const int idx[4] = {cy * cols + cx, cy * cols + cx + 1, (cy + 1) * cols + cx, (cy + 1) * cols + cx + 1};
            const double phi[4] = {(1 - u) * (1 - v), u * (1 - v), (1 - u) * v, u * v};
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) A[size_t(idx[i]) * n + idx[j]] += s.weight * phi[i] * phi[j];
                bx[idx[i]] += s.weight * phi[i] * s.tx;
                by[idx[i]] += s.weight * phi[i] * s.ty;
            }
            totalWeight += s.weight;
        }
        // Curvature penalty along rows and columns: (t[i-1] - 2 t[i] + t[i+1])^2, so empty nodes continue the trend.
        const double lambda = 0.05 * totalWeight / n;
        const auto penalty = [&](int a, int b, int c) {
            const int idx[3] = {a, b, c};
            const double coef[3] = {1.0, -2.0, 1.0};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) A[size_t(idx[i]) * n + idx[j]] += lambda * coef[i] * coef[j];
            }
        };
        for (int y = 0; y < rows; y++) {
            for (int x = 1; x + 1 < cols; x++) penalty(y * cols + x - 1, y * cols + x, y * cols + x + 1);
        }
        for (int x = 0; x < cols; x++) {
            for (int y = 1; y + 1 < rows; y++) penalty((y - 1) * cols + x, y * cols + x, (y + 1) * cols + x);
        }
        // A node no sample and no penalty reaches (only at 2 x 2) would leave a zero row; a hair of ridge guards it.
        for (int i = 0; i < n; i++) A[size_t(i) * n + i] += 1e-6 * totalWeight / n;
        if (!Solve(A, bx, by, n)) return false;
        out.tx.assign(n, 0.f);
        out.ty.assign(n, 0.f);
        for (int i = 0; i < n; i++) {
            out.tx[i] = float(bx[i]);
            out.ty[i] = float(by[i]);
        }
        return out.valid();
    }

    // The samples as pairs: each fresh, open moment's eye values against where the target was `lagSteps` records
    // earlier (the eyes trail a moving target, and VRChat's path adds its own delay).
    inline std::vector<Sample> Pair(const std::vector<Record>& records, int lagSteps, float pauseWeight) {
        std::vector<Sample> samples;
        for (size_t i = size_t(std::max(lagSteps, 0)); i < records.size(); i++) {
            const Record& now = records[i];
            if (!now.fresh || !now.open) continue;
            const Record& then = records[i - size_t(lagSteps)];
            samples.push_back({now.valueX, now.valueY, then.tanX, then.tanY, then.paused ? pauseWeight : 1.f});
        }
        return samples;
    }

    // How far the grid misses the samples, in degrees (weighted RMS over both axes).
    inline float ErrorDeg(const Grid& g, const std::vector<Sample>& samples, std::vector<float>* each = nullptr) {
        double sum = 0.0, weight = 0.0;
        if (each) each->clear();
        for (const Sample& s : samples) {
            float tx = 0, ty = 0;
            Lookup(g, s.vx, s.vy, tx, ty);
            const float dx = (std::atan(tx) - std::atan(s.tx)) * 180.f / Pi, dy = (std::atan(ty) - std::atan(s.ty)) * 180.f / Pi;
            const float e = std::sqrt(dx * dx + dy * dy);
            if (each) each->push_back(e);
            sum += double(s.weight) * e * e;
            weight += s.weight;
        }
        return weight > 0.0 ? float(std::sqrt(sum / weight)) : 0.f;
    }

    struct Result {
        Grid grid;
        int lagMs = 0;
        float errorDeg = 0.f; // After the fit, over the samples kept.
        int samples = 0;
        int dropped = 0;
    };

    // The whole thing: the delay that lines the eyes up with the target best (0..maxLagMs), the fit at that delay, the
    // samples that miss it by far more than the rest dropped, the fit again.
    inline bool Calibrate(const std::vector<Record>& records, float stepMs, int maxLagMs, Result& out) {
        constexpr int Cols = 5, Rows = 5;
        constexpr float PauseWeight = 3.f;
        int bestLag = -1;
        float bestError = 0.f;
        for (int lagMs = 0; lagMs <= maxLagMs; lagMs += 20) {
            const std::vector<Sample> samples = Pair(records, int(std::lround(lagMs / stepMs)), PauseWeight);
            Grid g;
            if (!FitGrid(samples, Cols, Rows, g)) continue;
            const float error = ErrorDeg(g, samples);
            if (bestLag < 0 || error < bestError) {
                bestLag = lagMs;
                bestError = error;
            }
        }
        if (bestLag < 0) return false;
        std::vector<Sample> samples = Pair(records, int(std::lround(bestLag / stepMs)), PauseWeight);
        Grid g;
        if (!FitGrid(samples, Cols, Rows, g)) return false;
        std::vector<float> errors;
        ErrorDeg(g, samples, &errors);
        std::vector<float> sorted = errors;
        std::sort(sorted.begin(), sorted.end());
        const float limit = std::max(3.f * sorted[sorted.size() / 2], 1.f);
        std::vector<Sample> kept;
        for (size_t i = 0; i < samples.size(); i++) {
            if (errors[i] <= limit) kept.push_back(samples[i]);
        }
        Grid refit;
        if (kept.size() >= 50 && FitGrid(kept, Cols, Rows, refit)) {
            out.grid = refit;
            out.samples = int(kept.size());
            out.dropped = int(samples.size() - kept.size());
            out.errorDeg = ErrorDeg(refit, kept);
        } else {
            out.grid = g;
            out.samples = int(samples.size());
            out.dropped = 0;
            out.errorDeg = ErrorDeg(g, samples);
        }
        out.lagMs = bestLag;
        return true;
    }

    // The map as text for the settings file: "cols x rows;x0,x1;y0,y1;tx:ty,tx:ty,..." - and back.
    inline std::string ToText(const Grid& g) {
        char item[64];
        snprintf(item, sizeof(item), "%dx%d;%.5f,%.5f;%.5f,%.5f;", g.cols, g.rows, g.x0, g.x1, g.y0, g.y1);
        std::string text = item;
        for (size_t i = 0; i < g.tx.size(); i++) {
            snprintf(item, sizeof(item), "%s%.5f:%.5f", i == 0 ? "" : ",", g.tx[i], g.ty[i]);
            text += item;
        }
        return text;
    }
    inline Grid FromText(const std::string& text) {
        Grid g;
        int cols = 0, rows = 0;
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        int consumed = 0;
        if (sscanf_s(text.c_str(), "%dx%d;%f,%f;%f,%f;%n", &cols, &rows, &x0, &x1, &y0, &y1, &consumed) != 6 || cols < 2 || rows < 2 || cols > 32 || rows > 32) return g;
        g.cols = cols;
        g.rows = rows;
        g.x0 = x0;
        g.x1 = x1;
        g.y0 = y0;
        g.y1 = y1;
        const char* p = text.c_str() + consumed;
        while (*p) {
            char* stop = nullptr;
            const float tx = std::strtof(p, &stop);
            if (stop == p || *stop != ':') break;
            p = stop + 1;
            const float ty = std::strtof(p, &stop);
            if (stop == p) break;
            g.tx.push_back(tx);
            g.ty.push_back(ty);
            p = *stop == ',' ? stop + 1 : stop;
        }
        if (!g.valid()) g = Grid{};
        return g;
    }

} // namespace gaze_mirror::grid
