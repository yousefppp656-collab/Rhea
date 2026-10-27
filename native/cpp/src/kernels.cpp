#include "rhea/rhea_c_api.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr size_t MAX_NDIM = 16;
constexpr size_t MAX_TENSOR_ELEMENTS = 64ULL * 1024ULL * 1024ULL; // 512 MB cap

bool valid_tensor(const rhea_TensorF64* t) {
    if (!t || !t->data || !t->shape || t->ndim == 0 || t->ndim > MAX_NDIM || t->numel == 0) {
        return false;
    }
    return true;
}

bool same_shape(const rhea_TensorF64* a, const rhea_TensorF64* b) {
    if (!valid_tensor(a) || !valid_tensor(b)) return false;
    if (a->ndim != b->ndim || a->numel != b->numel) return false;
    for (size_t i = 0; i < a->ndim; ++i) {
        if (a->shape[i] != b->shape[i]) return false;
    }
    return true;
}

bool safe_product(const size_t* shape, size_t ndim, size_t* out_numel) {
    if (!shape || ndim == 0 || ndim > MAX_NDIM) return false;
    size_t prod = 1;
    for (size_t i = 0; i < ndim; ++i) {
        if (shape[i] == 0) return false;
        if (prod > MAX_TENSOR_ELEMENTS / shape[i]) return false;
        prod *= shape[i];
    }
    *out_numel = prod;
    return true;
}

uint64_t splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double uniform_f64(uint64_t* state, double low, double high) {
    uint64_t r = splitmix64(state);
    double u = static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
    return low + (high - low) * u;
}

uint64_t fnv1a_update(uint64_t hash, const void* buf, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(buf);
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

extern "C" {

uint32_t rhea_abi_version(void) {
    return RHEA_ABI_VERSION;
}

const char* rhea_status_message(rhea_Status status) {
    switch (status) {
        case RHEA_OK: return "OK";
        case RHEA_ERR_NULL_POINTER: return "Null pointer argument";
        case RHEA_ERR_INVALID_SHAPE: return "Invalid or overflowing tensor shape";
        case RHEA_ERR_SHAPE_MISMATCH: return "Tensor shape mismatch";
        case RHEA_ERR_ALLOCATION_FAILED: return "Memory allocation failed";
        case RHEA_ERR_OUT_OF_BOUNDS: return "Index out of bounds";
        case RHEA_ERR_DIVISION_BY_ZERO: return "Division by zero";
        case RHEA_ERR_NUMERIC_INSTABILITY: return "NaN or Inf encountered";
        case RHEA_ERR_IO_FAILURE: return "File I/O or checkpoint verification failure";
        case RHEA_ERR_INVALID_ARGUMENT: return "Invalid argument value";
        default: return "Unknown status error";
    }
}

rhea_Status rhea_tensor_zeros(const size_t* shape, size_t ndim, rhea_TensorF64* out) {
    if (!shape || !out) return RHEA_ERR_NULL_POINTER;
    size_t numel = 0;
    if (!safe_product(shape, ndim, &numel)) return RHEA_ERR_INVALID_SHAPE;

    size_t* shape_copy = static_cast<size_t*>(std::calloc(ndim, sizeof(size_t)));
    if (!shape_copy) return RHEA_ERR_ALLOCATION_FAILED;

    double* data = static_cast<double*>(std::calloc(numel, sizeof(double)));
    if (!data) {
        std::free(shape_copy);
        return RHEA_ERR_ALLOCATION_FAILED;
    }

    std::memcpy(shape_copy, shape, ndim * sizeof(size_t));
    out->data = data;
    out->shape = shape_copy;
    out->ndim = ndim;
    out->numel = numel;
    return RHEA_OK;
}

rhea_Status rhea_tensor_from_buffer(const double* buffer, size_t numel, const size_t* shape, size_t ndim, rhea_TensorF64* out) {
    if (!buffer || !shape || !out) return RHEA_ERR_NULL_POINTER;
    size_t expected = 0;
    if (!safe_product(shape, ndim, &expected) || expected != numel) {
        return RHEA_ERR_INVALID_SHAPE;
    }
    for (size_t i = 0; i < numel; ++i) {
        if (!std::isfinite(buffer[i])) return RHEA_ERR_NUMERIC_INSTABILITY;
    }
    rhea_Status st = rhea_tensor_zeros(shape, ndim, out);
    if (st != RHEA_OK) return st;
    std::memcpy(out->data, buffer, numel * sizeof(double));
    return RHEA_OK;
}

rhea_Status rhea_tensor_clone(const rhea_TensorF64* src, rhea_TensorF64* out) {
    if (!valid_tensor(src) || !out) return RHEA_ERR_NULL_POINTER;
    return rhea_tensor_from_buffer(src->data, src->numel, src->shape, src->ndim, out);
}

void rhea_tensor_free(rhea_TensorF64* tensor) {
    if (!tensor) return;
    if (tensor->data) std::free(tensor->data);
    if (tensor->shape) std::free(tensor->shape);
    tensor->data = nullptr;
    tensor->shape = nullptr;
    tensor->ndim = 0;
    tensor->numel = 0;
}

rhea_Status rhea_tensor_add(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !valid_tensor(b) || !out) return RHEA_ERR_NULL_POINTER;
    if (!same_shape(a, b)) return RHEA_ERR_SHAPE_MISMATCH;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = a->data[i] + b->data[i];
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_sub(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !valid_tensor(b) || !out) return RHEA_ERR_NULL_POINTER;
    if (!same_shape(a, b)) return RHEA_ERR_SHAPE_MISMATCH;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = a->data[i] - b->data[i];
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_mul(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !valid_tensor(b) || !out) return RHEA_ERR_NULL_POINTER;
    if (!same_shape(a, b)) return RHEA_ERR_SHAPE_MISMATCH;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = a->data[i] * b->data[i];
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_scale(const rhea_TensorF64* a, double scalar, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !out) return RHEA_ERR_NULL_POINTER;
    if (!std::isfinite(scalar)) return RHEA_ERR_NUMERIC_INSTABILITY;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = a->data[i] * scalar;
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_matmul(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !valid_tensor(b) || !out) return RHEA_ERR_NULL_POINTER;
    if (a->ndim != 2 || b->ndim != 2) return RHEA_ERR_INVALID_SHAPE;
    size_t M = a->shape[0];
    size_t K = a->shape[1];
    size_t Kb = b->shape[0];
    size_t N = b->shape[1];
    if (K != Kb) return RHEA_ERR_SHAPE_MISMATCH;

    size_t out_shape[2] = {M, N};
    rhea_Status st = rhea_tensor_zeros(out_shape, 2, out);
    if (st != RHEA_OK) return st;

    constexpr size_t TILE = 32;
    for (size_t i0 = 0; i0 < M; i0 += TILE) {
        size_t i_max = std::min(i0 + TILE, M);
        for (size_t k0 = 0; k0 < K; k0 += TILE) {
            size_t k_max = std::min(k0 + TILE, K);
            for (size_t j0 = 0; j0 < N; j0 += TILE) {
                size_t j_max = std::min(j0 + TILE, N);
                for (size_t i = i0; i < i_max; ++i) {
                    for (size_t k = k0; k < k_max; ++k) {
                        double aik = a->data[i * K + k];
                        for (size_t j = j0; j < j_max; ++j) {
                            out->data[i * N + j] += aik * b->data[k * N + j];
                        }
                    }
                }
            }
        }
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_transpose_2d(const rhea_TensorF64* a, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !out) return RHEA_ERR_NULL_POINTER;
    if (a->ndim != 2) return RHEA_ERR_INVALID_SHAPE;
    size_t rows = a->shape[0];
    size_t cols = a->shape[1];
    size_t out_shape[2] = {cols, rows};
    rhea_Status st = rhea_tensor_zeros(out_shape, 2, out);
    if (st != RHEA_OK) return st;
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            out->data[c * rows + r] = a->data[r * cols + c];
        }
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_relu(const rhea_TensorF64* a, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !out) return RHEA_ERR_NULL_POINTER;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = a->data[i] > 0.0 ? a->data[i] : 0.0;
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_sigmoid(const rhea_TensorF64* a, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !out) return RHEA_ERR_NULL_POINTER;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        double x = a->data[i];
        if (x >= 0.0) {
            double z = std::exp(-x);
            out->data[i] = 1.0 / (1.0 + z);
        } else {
            double z = std::exp(x);
            out->data[i] = z / (1.0 + z);
        }
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_tanh(const rhea_TensorF64* a, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !out) return RHEA_ERR_NULL_POINTER;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = std::tanh(a->data[i]);
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_softmax_last_dim(const rhea_TensorF64* a, rhea_TensorF64* out) {
    if (!valid_tensor(a) || !out) return RHEA_ERR_NULL_POINTER;
    size_t last_dim = a->shape[a->ndim - 1];
    if (last_dim == 0) return RHEA_ERR_INVALID_SHAPE;
    rhea_Status st = rhea_tensor_zeros(a->shape, a->ndim, out);
    if (st != RHEA_OK) return st;

    size_t outer = a->numel / last_dim;
    for (size_t o = 0; o < outer; ++o) {
        const double* row_in = a->data + o * last_dim;
        double* row_out = out->data + o * last_dim;
        double max_val = row_in[0];
        for (size_t j = 1; j < last_dim; ++j) {
            if (row_in[j] > max_val) max_val = row_in[j];
        }
        double sum = 0.0;
        for (size_t j = 0; j < last_dim; ++j) {
            double e = std::exp(row_in[j] - max_val);
            row_out[j] = e;
            sum += e;
        }
        if (sum <= 0.0 || !std::isfinite(sum)) {
            rhea_tensor_free(out);
            return RHEA_ERR_NUMERIC_INSTABILITY;
        }
        for (size_t j = 0; j < last_dim; ++j) {
            row_out[j] /= sum;
        }
    }
    return RHEA_OK;
}

rhea_Status rhea_tensor_sum(const rhea_TensorF64* a, double* out_sum) {
    if (!valid_tensor(a) || !out_sum) return RHEA_ERR_NULL_POINTER;
    double s = 0.0;
    for (size_t i = 0; i < a->numel; ++i) s += a->data[i];
    *out_sum = s;
    return RHEA_OK;
}

rhea_Status rhea_tensor_mean(const rhea_TensorF64* a, double* out_mean) {
    if (!valid_tensor(a) || !out_mean) return RHEA_ERR_NULL_POINTER;
    double s = 0.0;
    rhea_Status st = rhea_tensor_sum(a, &s);
    if (st != RHEA_OK) return st;
    *out_mean = s / static_cast<double>(a->numel);
    return RHEA_OK;
}

rhea_Status rhea_tensor_l2_norm(const rhea_TensorF64* a, double* out_norm) {
    if (!valid_tensor(a) || !out_norm) return RHEA_ERR_NULL_POINTER;
    double sq = 0.0;
    for (size_t i = 0; i < a->numel; ++i) sq += a->data[i] * a->data[i];
    *out_norm = std::sqrt(sq);
    return RHEA_OK;
}

rhea_Status rhea_tensor_dot(const rhea_TensorF64* a, const rhea_TensorF64* b, double* out_dot) {
    if (!valid_tensor(a) || !valid_tensor(b) || !out_dot) return RHEA_ERR_NULL_POINTER;
    if (a->numel != b->numel) return RHEA_ERR_SHAPE_MISMATCH;
    double d = 0.0;
    for (size_t i = 0; i < a->numel; ++i) d += a->data[i] * b->data[i];
    *out_dot = d;
    return RHEA_OK;
}

rhea_Status rhea_mse_loss_and_grad(
    const rhea_TensorF64* predictions,
    const rhea_TensorF64* targets,
    double* out_loss,
    rhea_TensorF64* out_grad
) {
    if (!valid_tensor(predictions) || !valid_tensor(targets) || !out_loss || !out_grad) {
        return RHEA_ERR_NULL_POINTER;
    }
    if (!same_shape(predictions, targets)) return RHEA_ERR_SHAPE_MISMATCH;
    rhea_Status st = rhea_tensor_zeros(predictions->shape, predictions->ndim, out_grad);
    if (st != RHEA_OK) return st;

    double sum_sq = 0.0;
    double scale = 2.0 / static_cast<double>(predictions->numel);
    for (size_t i = 0; i < predictions->numel; ++i) {
        double diff = predictions->data[i] - targets->data[i];
        sum_sq += diff * diff;
        out_grad->data[i] = scale * diff;
    }
    *out_loss = sum_sq / static_cast<double>(predictions->numel);
    if (!std::isfinite(*out_loss)) {
        rhea_tensor_free(out_grad);
        return RHEA_ERR_NUMERIC_INSTABILITY;
    }
    return RHEA_OK;
}

rhea_Status rhea_linear_backward(
    const rhea_TensorF64* x,
    const rhea_TensorF64* w,
    const rhea_TensorF64* grad_out,
    rhea_TensorF64* grad_x,
    rhea_TensorF64* grad_w,
    rhea_TensorF64* grad_b
) {
    if (!valid_tensor(x) || !valid_tensor(w) || !valid_tensor(grad_out) || !grad_x || !grad_w || !grad_b) {
        return RHEA_ERR_NULL_POINTER;
    }
    if (x->ndim != 2 || w->ndim != 2 || grad_out->ndim != 2) return RHEA_ERR_INVALID_SHAPE;
    size_t B = x->shape[0];
    size_t I = x->shape[1];
    size_t Iw = w->shape[0];
    size_t O = w->shape[1];
    if (I != Iw || grad_out->shape[0] != B || grad_out->shape[1] != O) {
        return RHEA_ERR_SHAPE_MISMATCH;
    }

    rhea_TensorF64 w_t{nullptr, nullptr, 0, 0};
    rhea_TensorF64 x_t{nullptr, nullptr, 0, 0};
    rhea_Status st = rhea_tensor_transpose_2d(w, &w_t);
    if (st != RHEA_OK) return st;
    st = rhea_tensor_transpose_2d(x, &x_t);
    if (st != RHEA_OK) {
        rhea_tensor_free(&w_t);
        return st;
    }

    st = rhea_tensor_matmul(grad_out, &w_t, grad_x);
    if (st != RHEA_OK) {
        rhea_tensor_free(&w_t);
        rhea_tensor_free(&x_t);
        return st;
    }

    st = rhea_tensor_matmul(&x_t, grad_out, grad_w);
    rhea_tensor_free(&w_t);
    rhea_tensor_free(&x_t);
    if (st != RHEA_OK) {
        rhea_tensor_free(grad_x);
        return st;
    }

    size_t b_shape[2] = {1, O};
    st = rhea_tensor_zeros(b_shape, 2, grad_b);
    if (st != RHEA_OK) {
        rhea_tensor_free(grad_x);
        rhea_tensor_free(grad_w);
        return st;
    }
    for (size_t b = 0; b < B; ++b) {
        for (size_t o = 0; o < O; ++o) {
            grad_b->data[o] += grad_out->data[b * O + o];
        }
    }
    return RHEA_OK;
}

rhea_Status rhea_relu_backward(
    const rhea_TensorF64* x,
    const rhea_TensorF64* grad_out,
    rhea_TensorF64* grad_x
) {
    if (!valid_tensor(x) || !valid_tensor(grad_out) || !grad_x) return RHEA_ERR_NULL_POINTER;
    if (!same_shape(x, grad_out)) return RHEA_ERR_SHAPE_MISMATCH;
    rhea_Status st = rhea_tensor_zeros(x->shape, x->ndim, grad_x);
    if (st != RHEA_OK) return st;
    for (size_t i = 0; i < x->numel; ++i) {
        grad_x->data[i] = x->data[i] > 0.0 ? grad_out->data[i] : 0.0;
    }
    return RHEA_OK;
}

rhea_Status rhea_adam_step(
    rhea_TensorF64* param,
    const rhea_TensorF64* grad,
    rhea_TensorF64* m,
    rhea_TensorF64* v,
    uint64_t step,
    double lr,
    double beta1,
    double beta2,
    double eps
) {
    if (!valid_tensor(param) || !valid_tensor(grad) || !valid_tensor(m) || !valid_tensor(v)) {
        return RHEA_ERR_NULL_POINTER;
    }
    if (!same_shape(param, grad) || !same_shape(param, m) || !same_shape(param, v)) {
        return RHEA_ERR_SHAPE_MISMATCH;
    }
    if (step == 0 || lr <= 0.0 || beta1 < 0.0 || beta1 >= 1.0 || beta2 < 0.0 || beta2 >= 1.0 || eps <= 0.0) {
        return RHEA_ERR_INVALID_ARGUMENT;
    }
    double bias_correction1 = 1.0 - std::pow(beta1, static_cast<double>(step));
    double bias_correction2 = 1.0 - std::pow(beta2, static_cast<double>(step));
    for (size_t i = 0; i < param->numel; ++i) {
        double g = grad->data[i];
        if (!std::isfinite(g)) return RHEA_ERR_NUMERIC_INSTABILITY;
        m->data[i] = beta1 * m->data[i] + (1.0 - beta1) * g;
        v->data[i] = beta2 * v->data[i] + (1.0 - beta2) * (g * g);
        double m_hat = m->data[i] / bias_correction1;
        double v_hat = v->data[i] / bias_correction2;
        param->data[i] -= lr * m_hat / (std::sqrt(v_hat) + eps);
    }
    return RHEA_OK;
}

rhea_Status rhea_mlp_create(
    size_t in_dim,
    size_t hidden_dim,
    size_t out_dim,
    uint64_t seed,
    rhea_MLP2Layer* out_mlp
) {
    if (!out_mlp) return RHEA_ERR_NULL_POINTER;
    if (in_dim == 0 || hidden_dim == 0 || out_dim == 0) return RHEA_ERR_INVALID_SHAPE;
    std::memset(out_mlp, 0, sizeof(rhea_MLP2Layer));
    out_mlp->in_dim = in_dim;
    out_mlp->hidden_dim = hidden_dim;
    out_mlp->out_dim = out_dim;
    out_mlp->step = 0;

    size_t s_w1[2] = {in_dim, hidden_dim};
    size_t s_b1[2] = {1, hidden_dim};
    size_t s_w2[2] = {hidden_dim, out_dim};
    size_t s_b2[2] = {1, out_dim};

    if (rhea_tensor_zeros(s_w1, 2, &out_mlp->w1) != RHEA_OK ||
        rhea_tensor_zeros(s_b1, 2, &out_mlp->b1) != RHEA_OK ||
        rhea_tensor_zeros(s_w2, 2, &out_mlp->w2) != RHEA_OK ||
        rhea_tensor_zeros(s_b2, 2, &out_mlp->b2) != RHEA_OK ||
        rhea_tensor_zeros(s_w1, 2, &out_mlp->m_w1) != RHEA_OK ||
        rhea_tensor_zeros(s_w1, 2, &out_mlp->v_w1) != RHEA_OK ||
        rhea_tensor_zeros(s_b1, 2, &out_mlp->m_b1) != RHEA_OK ||
        rhea_tensor_zeros(s_b1, 2, &out_mlp->v_b1) != RHEA_OK ||
        rhea_tensor_zeros(s_w2, 2, &out_mlp->m_w2) != RHEA_OK ||
        rhea_tensor_zeros(s_w2, 2, &out_mlp->v_w2) != RHEA_OK ||
        rhea_tensor_zeros(s_b2, 2, &out_mlp->m_b2) != RHEA_OK ||
        rhea_tensor_zeros(s_b2, 2, &out_mlp->v_b2) != RHEA_OK) {
        rhea_mlp_free(out_mlp);
        return RHEA_ERR_ALLOCATION_FAILED;
    }

    uint64_t rng = seed ? seed : 42ULL;
    double limit1 = std::sqrt(6.0 / static_cast<double>(in_dim + hidden_dim));
    for (size_t i = 0; i < out_mlp->w1.numel; ++i) {
        out_mlp->w1.data[i] = uniform_f64(&rng, -limit1, limit1);
    }
    double limit2 = std::sqrt(6.0 / static_cast<double>(hidden_dim + out_dim));
    for (size_t i = 0; i < out_mlp->w2.numel; ++i) {
        out_mlp->w2.data[i] = uniform_f64(&rng, -limit2, limit2);
    }
    return RHEA_OK;
}

void rhea_mlp_free(rhea_MLP2Layer* mlp) {
    if (!mlp) return;
    rhea_tensor_free(&mlp->w1);
    rhea_tensor_free(&mlp->b1);
    rhea_tensor_free(&mlp->w2);
    rhea_tensor_free(&mlp->b2);
    rhea_tensor_free(&mlp->m_w1);
    rhea_tensor_free(&mlp->v_w1);
    rhea_tensor_free(&mlp->m_b1);
    rhea_tensor_free(&mlp->v_b1);
    rhea_tensor_free(&mlp->m_w2);
    rhea_tensor_free(&mlp->v_w2);
    rhea_tensor_free(&mlp->m_b2);
    rhea_tensor_free(&mlp->v_b2);
    std::memset(mlp, 0, sizeof(rhea_MLP2Layer));
}

static void add_bias_2d_inplace(rhea_TensorF64* mat, const rhea_TensorF64* bias) {
    size_t B = mat->shape[0];
    size_t C = mat->shape[1];
    for (size_t b = 0; b < B; ++b) {
        for (size_t c = 0; c < C; ++c) {
            mat->data[b * C + c] += bias->data[c];
        }
    }
}

rhea_Status rhea_mlp_forward(
    const rhea_MLP2Layer* mlp,
    const rhea_TensorF64* x,
    rhea_TensorF64* out_pred
) {
    if (!mlp || !valid_tensor(x) || !out_pred) return RHEA_ERR_NULL_POINTER;
    if (x->ndim != 2 || x->shape[1] != mlp->in_dim) return RHEA_ERR_SHAPE_MISMATCH;

    rhea_TensorF64 z1{nullptr, nullptr, 0, 0};
    rhea_TensorF64 a1{nullptr, nullptr, 0, 0};
    rhea_Status st = rhea_tensor_matmul(x, &mlp->w1, &z1);
    if (st != RHEA_OK) return st;
    add_bias_2d_inplace(&z1, &mlp->b1);

    st = rhea_tensor_relu(&z1, &a1);
    rhea_tensor_free(&z1);
    if (st != RHEA_OK) return st;

    st = rhea_tensor_matmul(&a1, &mlp->w2, out_pred);
    rhea_tensor_free(&a1);
    if (st != RHEA_OK) return st;
    add_bias_2d_inplace(out_pred, &mlp->b2);
    return RHEA_OK;
}

rhea_Status rhea_mlp_train_step(
    rhea_MLP2Layer* mlp,
    const rhea_TensorF64* x,
    const rhea_TensorF64* targets,
    double lr,
    double* out_loss
) {
    if (!mlp || !valid_tensor(x) || !valid_tensor(targets) || !out_loss) return RHEA_ERR_NULL_POINTER;
    if (x->ndim != 2 || targets->ndim != 2) return RHEA_ERR_INVALID_SHAPE;
    if (x->shape[0] != targets->shape[0] || x->shape[1] != mlp->in_dim || targets->shape[1] != mlp->out_dim) {
        return RHEA_ERR_SHAPE_MISMATCH;
    }

    rhea_TensorF64 z1{nullptr, nullptr, 0, 0};
    rhea_TensorF64 a1{nullptr, nullptr, 0, 0};
    rhea_TensorF64 pred{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_pred{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_a1{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_w2{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_b2{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_z1{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_x{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_w1{nullptr, nullptr, 0, 0};
    rhea_TensorF64 grad_b1{nullptr, nullptr, 0, 0};

    rhea_Status st = rhea_tensor_matmul(x, &mlp->w1, &z1);
    if (st != RHEA_OK) return st;
    add_bias_2d_inplace(&z1, &mlp->b1);

    st = rhea_tensor_relu(&z1, &a1);
    if (st != RHEA_OK) { rhea_tensor_free(&z1); return st; }

    st = rhea_tensor_matmul(&a1, &mlp->w2, &pred);
    if (st != RHEA_OK) { rhea_tensor_free(&z1); rhea_tensor_free(&a1); return st; }
    add_bias_2d_inplace(&pred, &mlp->b2);

    st = rhea_mse_loss_and_grad(&pred, targets, out_loss, &grad_pred);
    if (st != RHEA_OK) {
        rhea_tensor_free(&z1); rhea_tensor_free(&a1); rhea_tensor_free(&pred);
        return st;
    }

    st = rhea_linear_backward(&a1, &mlp->w2, &grad_pred, &grad_a1, &grad_w2, &grad_b2);
    if (st != RHEA_OK) {
        rhea_tensor_free(&z1); rhea_tensor_free(&a1); rhea_tensor_free(&pred); rhea_tensor_free(&grad_pred);
        return st;
    }

    st = rhea_relu_backward(&z1, &grad_a1, &grad_z1);
    if (st != RHEA_OK) {
        rhea_tensor_free(&z1); rhea_tensor_free(&a1); rhea_tensor_free(&pred); rhea_tensor_free(&grad_pred);
        rhea_tensor_free(&grad_a1); rhea_tensor_free(&grad_w2); rhea_tensor_free(&grad_b2);
        return st;
    }

    st = rhea_linear_backward(x, &mlp->w1, &grad_z1, &grad_x, &grad_w1, &grad_b1);
    if (st == RHEA_OK) {
        mlp->step += 1;
        st = rhea_adam_step(&mlp->w1, &grad_w1, &mlp->m_w1, &mlp->v_w1, mlp->step, lr, 0.9, 0.999, 1e-8);
        if (st == RHEA_OK) st = rhea_adam_step(&mlp->b1, &grad_b1, &mlp->m_b1, &mlp->v_b1, mlp->step, lr, 0.9, 0.999, 1e-8);
        if (st == RHEA_OK) st = rhea_adam_step(&mlp->w2, &grad_w2, &mlp->m_w2, &mlp->v_w2, mlp->step, lr, 0.9, 0.999, 1e-8);
        if (st == RHEA_OK) st = rhea_adam_step(&mlp->b2, &grad_b2, &mlp->m_b2, &mlp->v_b2, mlp->step, lr, 0.9, 0.999, 1e-8);
    }

    rhea_tensor_free(&z1);
    rhea_tensor_free(&a1);
    rhea_tensor_free(&pred);
    rhea_tensor_free(&grad_pred);
    rhea_tensor_free(&grad_a1);
    rhea_tensor_free(&grad_w2);
    rhea_tensor_free(&grad_b2);
    rhea_tensor_free(&grad_z1);
    rhea_tensor_free(&grad_x);
    rhea_tensor_free(&grad_w1);
    rhea_tensor_free(&grad_b1);
    return st;
}

rhea_Status rhea_mlp_train_epochs(
    rhea_MLP2Layer* mlp,
    const rhea_TensorF64* x,
    const rhea_TensorF64* targets,
    double lr,
    size_t epochs,
    double* out_initial_loss,
    double* out_final_loss
) {
    if (!mlp || !valid_tensor(x) || !valid_tensor(targets) || !out_initial_loss || !out_final_loss) {
        return RHEA_ERR_NULL_POINTER;
    }
    if (epochs == 0) return RHEA_ERR_INVALID_ARGUMENT;
    double loss = 0.0;
    for (size_t e = 0; e < epochs; ++e) {
        rhea_Status st = rhea_mlp_train_step(mlp, x, targets, lr, &loss);
        if (st != RHEA_OK) return st;
        if (e == 0) *out_initial_loss = loss;
    }
    *out_final_loss = loss;
    return RHEA_OK;
}

rhea_Status rhea_mlp_save_checkpoint(const rhea_MLP2Layer* mlp, const char* filepath) {
    if (!mlp || !filepath) return RHEA_ERR_NULL_POINTER;
    FILE* f = std::fopen(filepath, "wb");
    if (!f) return RHEA_ERR_IO_FAILURE;

    const char magic[4] = {'R', 'M', 'L', 'P'};
    uint32_t version = 1;
    uint64_t dims[4] = {
        static_cast<uint64_t>(mlp->in_dim),
        static_cast<uint64_t>(mlp->hidden_dim),
        static_cast<uint64_t>(mlp->out_dim),
        mlp->step
    };

    uint64_t hash = 14695981039346656037ULL;
    hash = fnv1a_update(hash, magic, 4);
    hash = fnv1a_update(hash, &version, sizeof(uint32_t));
    hash = fnv1a_update(hash, dims, sizeof(dims));
    hash = fnv1a_update(hash, mlp->w1.data, mlp->w1.numel * sizeof(double));
    hash = fnv1a_update(hash, mlp->b1.data, mlp->b1.numel * sizeof(double));
    hash = fnv1a_update(hash, mlp->w2.data, mlp->w2.numel * sizeof(double));
    hash = fnv1a_update(hash, mlp->b2.data, mlp->b2.numel * sizeof(double));

    bool ok = true;
    ok = ok && (std::fwrite(magic, 1, 4, f) == 4);
    ok = ok && (std::fwrite(&version, sizeof(uint32_t), 1, f) == 1);
    ok = ok && (std::fwrite(dims, sizeof(uint64_t), 4, f) == 4);
    ok = ok && (std::fwrite(mlp->w1.data, sizeof(double), mlp->w1.numel, f) == mlp->w1.numel);
    ok = ok && (std::fwrite(mlp->b1.data, sizeof(double), mlp->b1.numel, f) == mlp->b1.numel);
    ok = ok && (std::fwrite(mlp->w2.data, sizeof(double), mlp->w2.numel, f) == mlp->w2.numel);
    ok = ok && (std::fwrite(mlp->b2.data, sizeof(double), mlp->b2.numel, f) == mlp->b2.numel);
    ok = ok && (std::fwrite(&hash, sizeof(uint64_t), 1, f) == 1);
    std::fclose(f);
    return ok ? RHEA_OK : RHEA_ERR_IO_FAILURE;
}

rhea_Status rhea_mlp_load_checkpoint(rhea_MLP2Layer* out_mlp, const char* filepath) {
    if (!out_mlp || !filepath) return RHEA_ERR_NULL_POINTER;
    FILE* f = std::fopen(filepath, "rb");
    if (!f) return RHEA_ERR_IO_FAILURE;

    char magic[4] = {0};
    uint32_t version = 0;
    uint64_t dims[4] = {0};
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "RMLP", 4) != 0 ||
        std::fread(&version, sizeof(uint32_t), 1, f) != 1 || version != 1 ||
        std::fread(dims, sizeof(uint64_t), 4, f) != 4) {
        std::fclose(f);
        return RHEA_ERR_IO_FAILURE;
    }

    if (dims[0] == 0 || dims[1] == 0 || dims[2] == 0 ||
        dims[0] > 16384 || dims[1] > 16384 || dims[2] > 16384) {
        std::fclose(f);
        return RHEA_ERR_INVALID_SHAPE;
    }

    rhea_Status st = rhea_mlp_create(
        static_cast<size_t>(dims[0]),
        static_cast<size_t>(dims[1]),
        static_cast<size_t>(dims[2]),
        1ULL,
        out_mlp
    );
    if (st != RHEA_OK) {
        std::fclose(f);
        return st;
    }
    out_mlp->step = dims[3];

    uint64_t expected_hash = 0;
    bool ok = true;
    ok = ok && (std::fread(out_mlp->w1.data, sizeof(double), out_mlp->w1.numel, f) == out_mlp->w1.numel);
    ok = ok && (std::fread(out_mlp->b1.data, sizeof(double), out_mlp->b1.numel, f) == out_mlp->b1.numel);
    ok = ok && (std::fread(out_mlp->w2.data, sizeof(double), out_mlp->w2.numel, f) == out_mlp->w2.numel);
    ok = ok && (std::fread(out_mlp->b2.data, sizeof(double), out_mlp->b2.numel, f) == out_mlp->b2.numel);
    ok = ok && (std::fread(&expected_hash, sizeof(uint64_t), 1, f) == 1);

    int extra = std::fgetc(f);
    std::fclose(f);
    if (!ok || extra != EOF) {
        rhea_mlp_free(out_mlp);
        return RHEA_ERR_IO_FAILURE;
    }

    auto all_finite = [](const rhea_TensorF64& t) {
        for (size_t i = 0; i < t.numel; ++i) {
            if (!std::isfinite(t.data[i])) return false;
        }
        return true;
    };
    if (!all_finite(out_mlp->w1) || !all_finite(out_mlp->b1) ||
        !all_finite(out_mlp->w2) || !all_finite(out_mlp->b2)) {
        rhea_mlp_free(out_mlp);
        return RHEA_ERR_NUMERIC_INSTABILITY;
    }

    uint64_t actual_hash = 14695981039346656037ULL;
    actual_hash = fnv1a_update(actual_hash, magic, 4);
    actual_hash = fnv1a_update(actual_hash, &version, sizeof(uint32_t));
    actual_hash = fnv1a_update(actual_hash, dims, sizeof(dims));
    actual_hash = fnv1a_update(actual_hash, out_mlp->w1.data, out_mlp->w1.numel * sizeof(double));
    actual_hash = fnv1a_update(actual_hash, out_mlp->b1.data, out_mlp->b1.numel * sizeof(double));
    actual_hash = fnv1a_update(actual_hash, out_mlp->w2.data, out_mlp->w2.numel * sizeof(double));
    actual_hash = fnv1a_update(actual_hash, out_mlp->b2.data, out_mlp->b2.numel * sizeof(double));
    if (actual_hash != expected_hash) {
        rhea_mlp_free(out_mlp);
        return RHEA_ERR_IO_FAILURE;
    }

    return RHEA_OK;
}

} // extern "C"
