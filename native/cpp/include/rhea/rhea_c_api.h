#ifndef RHEA_C_API_H
#define RHEA_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RHEA_ABI_VERSION 2

typedef enum rhea_Status {
    RHEA_OK = 0,
    RHEA_ERR_NULL_POINTER = 1,
    RHEA_ERR_INVALID_SHAPE = 2,
    RHEA_ERR_SHAPE_MISMATCH = 3,
    RHEA_ERR_ALLOCATION_FAILED = 4,
    RHEA_ERR_OUT_OF_BOUNDS = 5,
    RHEA_ERR_DIVISION_BY_ZERO = 6,
    RHEA_ERR_NUMERIC_INSTABILITY = 7,
    RHEA_ERR_IO_FAILURE = 8,
    RHEA_ERR_INVALID_ARGUMENT = 9
} rhea_Status;

typedef struct rhea_TensorF64 {
    double* data;
    size_t* shape;
    size_t ndim;
    size_t numel;
} rhea_TensorF64;

typedef struct rhea_MLP2Layer {
    size_t in_dim;
    size_t hidden_dim;
    size_t out_dim;
    rhea_TensorF64 w1;
    rhea_TensorF64 b1;
    rhea_TensorF64 w2;
    rhea_TensorF64 b2;
    rhea_TensorF64 m_w1, v_w1;
    rhea_TensorF64 m_b1, v_b1;
    rhea_TensorF64 m_w2, v_w2;
    rhea_TensorF64 m_b2, v_b2;
    uint64_t step;
} rhea_MLP2Layer;

uint32_t rhea_abi_version(void);
const char* rhea_status_message(rhea_Status status);

rhea_Status rhea_tensor_zeros(const size_t* shape, size_t ndim, rhea_TensorF64* out);
rhea_Status rhea_tensor_from_buffer(const double* buffer, size_t numel, const size_t* shape, size_t ndim, rhea_TensorF64* out);
rhea_Status rhea_tensor_clone(const rhea_TensorF64* src, rhea_TensorF64* out);
void rhea_tensor_free(rhea_TensorF64* tensor);

rhea_Status rhea_tensor_add(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out);
rhea_Status rhea_tensor_sub(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out);
rhea_Status rhea_tensor_mul(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out);
rhea_Status rhea_tensor_scale(const rhea_TensorF64* a, double scalar, rhea_TensorF64* out);
rhea_Status rhea_tensor_matmul(const rhea_TensorF64* a, const rhea_TensorF64* b, rhea_TensorF64* out);
rhea_Status rhea_tensor_transpose_2d(const rhea_TensorF64* a, rhea_TensorF64* out);

rhea_Status rhea_tensor_relu(const rhea_TensorF64* a, rhea_TensorF64* out);
rhea_Status rhea_tensor_sigmoid(const rhea_TensorF64* a, rhea_TensorF64* out);
rhea_Status rhea_tensor_tanh(const rhea_TensorF64* a, rhea_TensorF64* out);
rhea_Status rhea_tensor_softmax_last_dim(const rhea_TensorF64* a, rhea_TensorF64* out);

rhea_Status rhea_tensor_sum(const rhea_TensorF64* a, double* out_sum);
rhea_Status rhea_tensor_mean(const rhea_TensorF64* a, double* out_mean);
rhea_Status rhea_tensor_l2_norm(const rhea_TensorF64* a, double* out_norm);
rhea_Status rhea_tensor_dot(const rhea_TensorF64* a, const rhea_TensorF64* b, double* out_dot);

rhea_Status rhea_mse_loss_and_grad(
    const rhea_TensorF64* predictions,
    const rhea_TensorF64* targets,
    double* out_loss,
    rhea_TensorF64* out_grad
);

rhea_Status rhea_linear_backward(
    const rhea_TensorF64* x,
    const rhea_TensorF64* w,
    const rhea_TensorF64* grad_out,
    rhea_TensorF64* grad_x,
    rhea_TensorF64* grad_w,
    rhea_TensorF64* grad_b
);

rhea_Status rhea_relu_backward(
    const rhea_TensorF64* x,
    const rhea_TensorF64* grad_out,
    rhea_TensorF64* grad_x
);

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
);

rhea_Status rhea_mlp_create(
    size_t in_dim,
    size_t hidden_dim,
    size_t out_dim,
    uint64_t seed,
    rhea_MLP2Layer* out_mlp
);

void rhea_mlp_free(rhea_MLP2Layer* mlp);

rhea_Status rhea_mlp_forward(
    const rhea_MLP2Layer* mlp,
    const rhea_TensorF64* x,
    rhea_TensorF64* out_pred
);

rhea_Status rhea_mlp_train_step(
    rhea_MLP2Layer* mlp,
    const rhea_TensorF64* x,
    const rhea_TensorF64* targets,
    double lr,
    double* out_loss
);

rhea_Status rhea_mlp_train_epochs(
    rhea_MLP2Layer* mlp,
    const rhea_TensorF64* x,
    const rhea_TensorF64* targets,
    double lr,
    size_t epochs,
    double* out_initial_loss,
    double* out_final_loss
);

rhea_Status rhea_mlp_save_checkpoint(const rhea_MLP2Layer* mlp, const char* filepath);
rhea_Status rhea_mlp_load_checkpoint(rhea_MLP2Layer* out_mlp, const char* filepath);

#ifdef __cplusplus
}
#endif

#endif // RHEA_C_API_H
