#include "rhea/rhea_c_api.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

int main() {
    assert(rhea_abi_version() == RHEA_ABI_VERSION);

    // 1. Tensor creation & elementwise ops
    size_t shape2x2[2] = {2, 2};
    double d1[4] = {1.0, 2.0, 3.0, 4.0};
    double d2[4] = {5.0, 6.0, 7.0, 8.0};
    rhea_TensorF64 a{}, b{}, c{};
    assert(rhea_tensor_from_buffer(d1, 4, shape2x2, 2, &a) == RHEA_OK);
    assert(rhea_tensor_from_buffer(d2, 4, shape2x2, 2, &b) == RHEA_OK);
    assert(rhea_tensor_add(&a, &b, &c) == RHEA_OK);
    assert(std::fabs(c.data[0] - 6.0) < 1e-12 && std::fabs(c.data[3] - 12.0) < 1e-12);
    rhea_tensor_free(&c);

    // 2. Matmul verification
    assert(rhea_tensor_matmul(&a, &b, &c) == RHEA_OK);
    assert(std::fabs(c.data[0] - 19.0) < 1e-12);
    assert(std::fabs(c.data[1] - 22.0) < 1e-12);
    assert(std::fabs(c.data[2] - 43.0) < 1e-12);
    assert(std::fabs(c.data[3] - 50.0) < 1e-12);
    rhea_tensor_free(&c);

    // 3. Softmax numerical stability under extreme logits
    double logits[3] = {1000.0, 1001.0, 1002.0};
    size_t shape1x3[2] = {1, 3};
    rhea_TensorF64 t_logits{}, t_probs{};
    assert(rhea_tensor_from_buffer(logits, 3, shape1x3, 2, &t_logits) == RHEA_OK);
    assert(rhea_tensor_softmax_last_dim(&t_logits, &t_probs) == RHEA_OK);
    double prob_sum = t_probs.data[0] + t_probs.data[1] + t_probs.data[2];
    assert(std::fabs(prob_sum - 1.0) < 1e-12);
    assert(t_probs.data[2] > t_probs.data[1] && t_probs.data[1] > t_probs.data[0]);
    rhea_tensor_free(&t_logits);
    rhea_tensor_free(&t_probs);

    // 4. Reverse-mode autodiff linear + ReLU + MSE loss gradient check
    double x_buf[2] = {2.0, -1.0};
    double w_buf[2] = {3.0, 4.0};
    double go_buf[1] = {1.0};
    size_t s_1x2[2] = {1, 2};
    size_t s_2x1[2] = {2, 1};
    size_t s_1x1[2] = {1, 1};
    rhea_TensorF64 tx{}, tw{}, tgo{}, gx{}, gw{}, gb{};
    assert(rhea_tensor_from_buffer(x_buf, 2, s_1x2, 2, &tx) == RHEA_OK);
    assert(rhea_tensor_from_buffer(w_buf, 2, s_2x1, 2, &tw) == RHEA_OK);
    assert(rhea_tensor_from_buffer(go_buf, 1, s_1x1, 2, &tgo) == RHEA_OK);
    assert(rhea_linear_backward(&tx, &tw, &tgo, &gx, &gw, &gb) == RHEA_OK);
    assert(std::fabs(gx.data[0] - 3.0) < 1e-12 && std::fabs(gx.data[1] - 4.0) < 1e-12);
    assert(std::fabs(gw.data[0] - 2.0) < 1e-12 && std::fabs(gw.data[1] - (-1.0)) < 1e-12);
    assert(std::fabs(gb.data[0] - 1.0) < 1e-12);
    rhea_tensor_free(&tx); rhea_tensor_free(&tw); rhea_tensor_free(&tgo);
    rhea_tensor_free(&gx); rhea_tensor_free(&gw); rhea_tensor_free(&gb);

    // 5. 2-Layer MLP XOR training convergence & checkpoint save/load + corruption tamper rejection
    double xor_x[8] = {0,0, 0,1, 1,0, 1,1};
    double xor_y[4] = {0, 1, 1, 0};
    size_t sx[2] = {4, 2};
    size_t sy[2] = {4, 1};
    rhea_TensorF64 X{}, Y{};
    assert(rhea_tensor_from_buffer(xor_x, 8, sx, 2, &X) == RHEA_OK);
    assert(rhea_tensor_from_buffer(xor_y, 4, sy, 2, &Y) == RHEA_OK);

    rhea_MLP2Layer mlp{};
    assert(rhea_mlp_create(2, 16, 1, 1337ULL, &mlp) == RHEA_OK);
    double init_loss = 0.0, final_loss = 0.0;
    assert(rhea_mlp_train_epochs(&mlp, &X, &Y, 0.03, 500, &init_loss, &final_loss) == RHEA_OK);
    assert(final_loss < init_loss * 0.05);
    assert(final_loss < 0.01);

    const char* ckpt = "/tmp/rhea_test_mlp.ckpt";
    assert(rhea_mlp_save_checkpoint(&mlp, ckpt) == RHEA_OK);
    rhea_MLP2Layer loaded{};
    assert(rhea_mlp_load_checkpoint(&loaded, ckpt) == RHEA_OK);
    rhea_TensorF64 p1{}, p2{};
    assert(rhea_mlp_forward(&mlp, &X, &p1) == RHEA_OK);
    assert(rhea_mlp_forward(&loaded, &X, &p2) == RHEA_OK);
    for (size_t i = 0; i < 4; ++i) {
        assert(std::fabs(p1.data[i] - p2.data[i]) < 1e-12);
    }

    // Corrupt 1 byte in checkpoint -> must fail FNV-1a integrity verification
    FILE* f = std::fopen(ckpt, "r+b");
    assert(f != nullptr);
    std::fseek(f, 44, SEEK_SET);
    uint8_t b_corrupt = 0xFF;
    std::fwrite(&b_corrupt, 1, 1, f);
    std::fclose(f);
    rhea_MLP2Layer bad{};
    assert(rhea_mlp_load_checkpoint(&bad, ckpt) != RHEA_OK);
    std::remove(ckpt);

    rhea_tensor_free(&p1);
    rhea_tensor_free(&p2);
    rhea_mlp_free(&mlp);
    rhea_mlp_free(&loaded);
    rhea_tensor_free(&X);
    rhea_tensor_free(&Y);
    rhea_tensor_free(&a);
    rhea_tensor_free(&b);

    std::printf("[PASS] All C++20 native kernel, autodiff, Adam, and checkpoint integrity tests passed.\n");
    return 0;
}
