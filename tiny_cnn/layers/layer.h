/*
    Copyright (c) 2013, Taiga Nomi
    All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY 
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND 
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <sstream>
#include <iomanip>
#include <memory>
#include <stdio.h>
#include "tiny_cnn/util/util.h"
#include "tiny_cnn/util/product.h"
#include "tiny_cnn/util/image.h"
#include "tiny_cnn/util/weight_init.h"

#include "tiny_cnn/activations/activation_function.h"

namespace tiny_cnn {


// base class of all kind of NN layers
class layer_base {
public:
    friend void connection_mismatch(const layer_base& from, const layer_base& to);

    virtual ~layer_base() = default;

    layer_base(cnn_size_t in_dim, cnn_size_t out_dim, size_t weight_dim, size_t bias_dim)
        : parallelize_(true), next_(nullptr), prev_(nullptr),
          weight_init_(std::make_shared<weight_init::xavier>()),
          bias_init_(std::make_shared<weight_init::constant>(float_t(0)))
    {
        set_size(in_dim, out_dim, weight_dim, bias_dim);
        set_worker_count(CNN_TASK_SIZE);
    }

    layer_base(const layer_base&) = default;
    layer_base &operator =(const layer_base&) = default;

#if !defined(_MSC_VER) || (_MSC_VER >= 1900) // default generation of move constructor is unsupported in VS2013
    layer_base(layer_base&&) = default;
    layer_base &operator = (layer_base&&) = default;
#endif

    void connect(std::shared_ptr<layer_base>& tail) {
        if (out_size() != 0 && tail->in_size() != out_size())
            connection_mismatch(*this, *tail);
        next_ = tail.get();
        tail->prev_ = this;
    }

    void set_parallelize(bool parallelize) {
        parallelize_ = parallelize;
    }

    // cannot call from ctor because of pure virtual function call fan_in_size().
    // so should call this function explicitly after ctor
    void init_weight() {
        weight_init_->fill(&W_, static_cast<cnn_size_t>(fan_in_size()),
                           static_cast<cnn_size_t>(fan_out_size()));
        bias_init_->fill(&b_, static_cast<cnn_size_t>(fan_in_size()),
                         static_cast<cnn_size_t>(fan_out_size()));

        std::fill(Whessian_.begin(), Whessian_.end(), float_t(0));
        std::fill(bhessian_.begin(), bhessian_.end(), float_t(0));
        clear_diff();
    }

    void divide_hessian(int denominator) {
        for (auto& w : Whessian_) w /= denominator;
        for (auto& b : bhessian_) b /= denominator;
    }

    /////////////////////////////////////////////////////////////////////////
    // getter

    const vec_t& output(cnn_size_t worker_index) const { return worker_storage_[worker_index].output_; }
    const vec_t& delta(cnn_size_t worker_index) const { return worker_storage_[worker_index].prev_delta_; }
    vec_t& weight() { return W_; }
    vec_t& bias() { return b_; }
    vec_t& weight_diff(cnn_size_t index) { return worker_storage_[index].dW_; }
    vec_t& bias_diff(cnn_size_t index) { return worker_storage_[index].db_; }
    bool is_exploded() const { return has_infinite(W_) || has_infinite(b_); }
    layer_base* next() { return next_; }
    layer_base* prev() { return prev_; }

    ///< input dimension
    virtual cnn_size_t in_size() const { return in_size_; }

    cnn_size_t in_dim() const { return in_size(); }

    ///< output dimension
    virtual cnn_size_t out_size() const { return out_size_; }

    cnn_size_t out_dim() const { return out_size(); }

    ///< number of parameters
    virtual size_t param_size() const { return W_.size() + b_.size(); }

    ///< number of incoming connections for each output unit
    virtual size_t fan_in_size() const = 0;

    ///< number of outgoing connections for each input unit
    virtual size_t fan_out_size() const = 0;

    ///< number of connections
    virtual size_t connection_size() const = 0;

    ///< input shape(width x height x depth)
    virtual index3d<cnn_size_t> in_shape() const { return index3d<cnn_size_t>(in_size(), 1, 1); }

    ///< output shape(width x height x depth)
    virtual index3d<cnn_size_t> out_shape() const { return index3d<cnn_size_t>(out_size(), 1, 1); }

    ///< name of layer. should be unique for each concrete class
    virtual std::string layer_type() const = 0;

    virtual activation::function& activation_function() = 0;

    /////////////////////////////////////////////////////////////////////////
    // setter
    template <typename WeightInit>
    layer_base& weight_init(const WeightInit& f) { weight_init_ = std::make_shared<WeightInit>(f); return *this; }

    template <typename BiasInit>
    layer_base& bias_init(const BiasInit& f) { bias_init_ = std::make_shared<BiasInit>(f); return *this; }

    template <typename WeightInit>
    layer_base& weight_init(std::shared_ptr<WeightInit> f) { weight_init_ = f; return *this; }

    template <typename BiasInit>
    layer_base& bias_init(std::shared_ptr<BiasInit> f) { bias_init_ = f; return *this; }

    /////////////////////////////////////////////////////////////////////////
    // save/load
    virtual void save(std::ostream& os) const {
        if (is_exploded()) throw nn_error("failed to save weights because of infinite weight");
        for (auto w : W_) os << w << " ";
        for (auto b : b_) os << b << " ";
    }

    virtual void load(std::istream& is) {
        for (auto& w : W_) is >> w;
        for (auto& b : b_) is >> b;
    }
    
    virtual void load(std::vector<double> weights, int& idx) {
		for (auto& w : W_) w = weights[idx++];
		for (auto& b : b_) b = weights[idx++];
	}

    /////////////////////////////////////////////////////////////////////////
    // visualize

    ///< visualize latest output of this layer
    ///< default implementation interpret output as 1d-vector,
    ///< so "visual" layer(like convolutional layer) should override this for better visualization.
    virtual image<> output_to_image(size_t worker_index = 0) const {
        return vec2image<unsigned char>(worker_storage_[worker_index].output_);
    }

    /////////////////////////////////////////////////////////////////////////
    // fprop/bprop

    /**
     * return output vector
     * output vector must be stored to output_[worker_index]
     **/
    virtual const vec_t& forward_propagation(const vec_t& in, size_t worker_index) = 0;

    /**
     * return delta of previous layer (delta=\frac{dE}{da}, a=wx in fully-connected layer)
     * delta must be stored to prev_delta_[worker_index]
     **/
    virtual const vec_t& back_propagation(const vec_t& current_delta, size_t worker_index) = 0;

    /**
     * return delta2 of previous layer (delta2=\frac{d^2E}{da^2}, diagonal of hessian matrix)
     * it is never called if optimizer is hessian-free
     **/
    virtual const vec_t& back_propagation_2nd(const vec_t& current_delta2) = 0;

    // called afrer updating weight
    virtual void post_update() {}

    /**
     * notify changing context (train <=> test)
     **/
     virtual void set_context(net_phase ctx) { CNN_UNREFERENCED_PARAMETER(ctx); }

    template <typename Optimizer>
    void update_weight(Optimizer *o, cnn_size_t worker_size, cnn_size_t batch_size) {
        if (W_.empty()) return;

        merge(worker_size, batch_size);

        CNN_LOG_VECTOR(W_, "[W-before]");
        CNN_LOG_VECTOR(b_, "[db-before]");

        o->update(worker_storage_[0].dW_, Whessian_, W_);
        o->update(worker_storage_[0].db_, bhessian_, b_);

        CNN_LOG_VECTOR(W_, "[W-updated]");
        CNN_LOG_VECTOR(b_, "[db-updated]");

        clear_diff();
        post_update();
    }

    bool has_same_weights(const layer_base& rhs, float_t eps) const {
        if (W_.size() != rhs.W_.size() || b_.size() != rhs.b_.size())
            return false;

        for (size_t i = 0; i < W_.size(); i++)
          if (std::abs(W_[i] - rhs.W_[i]) > eps) return false;
        for (size_t i = 0; i < b_.size(); i++)
          if (std::abs(b_[i] - rhs.b_[i]) > eps) return false;

        return true;
    }

    virtual void set_worker_count(cnn_size_t worker_count) {
        if (worker_count == 0) {
            throw nn_error("worker_count cannot be zero!");
        }

        if (worker_count != worker_storage_.size()) {
            worker_storage_.resize(worker_count);

            // resize the vectors of each worker
            set_size(in_size_, out_size_, W_.size(), b_.size());
        }
    }

protected:
    cnn_size_t in_size_;
    cnn_size_t out_size_;
    bool parallelize_;

    layer_base* next_;
    layer_base* prev_;
    vec_t W_;          // weight vector
    vec_t b_;          // bias vector

    struct worker_specific_storage {
        vec_t a_;          // w * x
        vec_t output_;     // last output of current layer, set by fprop
        vec_t prev_delta_; // last delta of previous layer, set by bprop

        // worker's contribution to derivative of loss function with respect to weights of this layer
        vec_t dW_;

        // worker's contribution to derivative of loss function with respect to bias terms of this layer
        vec_t db_;
    };

    vec_t Whessian_; // diagonal terms of hessian matrix
    vec_t bhessian_;
    vec_t prev_delta2_; // d^2E/da^2
    std::shared_ptr<weight_init::function> weight_init_;
    std::shared_ptr<weight_init::function> bias_init_;

    const worker_specific_storage& get_worker_storage(cnn_size_t worker_index) const {
        return worker_storage_[worker_index];
    }

    worker_specific_storage& get_worker_storage(cnn_size_t worker_index) {
        return worker_storage_[worker_index];
    }

private:
    /** sums contributions to gradient (of the loss function with respect to weights and
        bias) as calculated by individual threads */
    void merge(cnn_size_t worker_size, cnn_size_t batch_size) {
        auto& ws = worker_storage_; // short-hand notation

        for (cnn_size_t i = 1; i < worker_size; i++)
            vectorize::reduce<float_t>(&ws[i].dW_[0],
                static_cast<cnn_size_t>(ws[i].dW_.size()), &ws[0].dW_[0]);
        for (cnn_size_t i = 1; i < worker_size; i++)
            vectorize::reduce<float_t>(&ws[i].db_[0],
                static_cast<cnn_size_t>(ws[i].db_.size()), &ws[0].db_[0]);

        std::transform(ws[0].dW_.begin(), ws[0].dW_.end(), ws[0].dW_.begin(), [&](float_t x) { return x / batch_size; });
        std::transform(ws[0].db_.begin(), ws[0].db_.end(), ws[0].db_.begin(), [&](float_t x) { return x / batch_size; });

        CNN_LOG_VECTOR(dW_[0], "[dW-merged]");
        CNN_LOG_VECTOR(db_[0], "[db-merged]");
    }

    void clear_diff() {
        for (auto& ws : worker_storage_) {
            std::fill(ws.dW_.begin(), ws.dW_.end(), float_t(0));
            std::fill(ws.db_.begin(), ws.db_.end(), float_t(0));
        }
    }

    void set_size(cnn_size_t in_dim, cnn_size_t out_dim, size_t weight_dim, size_t bias_dim) {
        in_size_ = in_dim;
        out_size_ = out_dim;

        W_.resize(weight_dim);
        b_.resize(bias_dim);
        Whessian_.resize(weight_dim);
        bhessian_.resize(bias_dim);
        prev_delta2_.resize(in_dim);

        for (auto& ws : worker_storage_) {
            ws.output_.resize(out_dim);
            ws.a_.resize(out_dim);
            ws.prev_delta_.resize(in_dim);
            ws.dW_.resize(weight_dim);
            ws.db_.resize(bias_dim);
        }
    }

    std::vector<worker_specific_storage> worker_storage_;
};

template<typename Activation>
class layer : public layer_base {
public:
    layer(cnn_size_t in_dim, cnn_size_t out_dim, size_t weight_dim, size_t bias_dim)
        : layer_base(in_dim, out_dim, weight_dim, bias_dim) {}

    activation::function& activation_function() override { return h_; }
protected:
    Activation h_;
};

template <typename Char, typename CharTraits>
std::basic_ostream<Char, CharTraits>& operator << (std::basic_ostream<Char, CharTraits>& os, const layer_base& v) {
    v.save(os);
    return os;
}

template <typename Char, typename CharTraits>
std::basic_istream<Char, CharTraits>& operator >> (std::basic_istream<Char, CharTraits>& os, layer_base& v) {
    v.load(os);
    return os;
}

// error message functions

inline void connection_mismatch(const layer_base& from, const layer_base& to) {
    std::ostringstream os;

    os << std::endl;
    os << "output size of Nth layer must be equal to input of (N+1)th layer" << std::endl;
    os << "layerN:   " << std::setw(12) << from.layer_type() << " in:" << from.in_size() << "(" << from.in_shape() << "), " << 
                                                "out:" << from.out_size() << "(" << from.out_shape() << ")" << std::endl;
    os << "layerN+1: " << std::setw(12) << to.layer_type() << " in:" << to.in_size() << "(" << to.in_shape() << "), " <<
                                             "out:" << to.out_size() << "(" << to.out_shape() << ")" << std::endl;
    os << from.out_size() << " != " << to.in_size() << std::endl;
    std::string detail_info = os.str();

    throw nn_error("layer dimension mismatch!" + detail_info);
}

inline void data_mismatch(const layer_base& layer, const vec_t& data) {
    std::ostringstream os;

    os << std::endl;
    os << "data dimension:    " << data.size() << std::endl;
    os << "network dimension: " << layer.in_size() << "(" << layer.layer_type() << ":" << layer.in_shape() << ")" << std::endl;

    std::string detail_info = os.str();

    throw nn_error("input dimension mismath!" + detail_info);
}

inline void pooling_size_mismatch(cnn_size_t in_width, cnn_size_t in_height, cnn_size_t pooling_size) {
    std::ostringstream os;

    os << std::endl;
    os << "WxH:" << in_width << "x" << in_height << std::endl;
    os << "pooling-size:" << pooling_size << std::endl;

    std::string detail_info = os.str();

    throw nn_error("width/height must be multiples of pooling size" + detail_info);
}

} // namespace tiny_cnn
