// Copyright © 2023-2024 Apple Inc.
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "mlx/allocator.h"
#include "mlx/primitives.h"
#include "mlx/transforms.h"
#include "mlx/transforms_impl.h"

namespace mlx::core {

// TODO how to set this?
constexpr int max_compile_size = 8;

bool is_unary(const Primitive& p) {
  // TODO fix this mess.
  return (
      typeid(p) == typeid(Abs) || typeid(p) == typeid(ArcCos) ||
      typeid(p) == typeid(ArcCosh) || typeid(p) == typeid(ArcSin) ||
      typeid(p) == typeid(ArcSinh) || typeid(p) == typeid(ArcTan) ||
      typeid(p) == typeid(ArcTanh) || typeid(p) == typeid(AsType) ||
      typeid(p) == typeid(Ceil) || typeid(p) == typeid(Copy) ||
      typeid(p) == typeid(Cos) || typeid(p) == typeid(Cosh) ||
      typeid(p) == typeid(Remainder) || typeid(p) == typeid(Erf) ||
      typeid(p) == typeid(ErfInv) || typeid(p) == typeid(Exp) ||
      typeid(p) == typeid(Floor) || typeid(p) == typeid(Log) ||
      typeid(p) == typeid(Log1p) || typeid(p) == typeid(LogicalNot) ||
      typeid(p) == typeid(Negative) || typeid(p) == typeid(Round) ||
      typeid(p) == typeid(Sigmoid) || typeid(p) == typeid(Sign) ||
      typeid(p) == typeid(Sin) || typeid(p) == typeid(Sinh) ||
      typeid(p) == typeid(Square) || typeid(p) == typeid(Sqrt) ||
      typeid(p) == typeid(Tan) || typeid(p) == typeid(Tanh));
}

bool is_binary(const Primitive& p) {
  // TODO fix this mess.
  return (
      typeid(p) == typeid(Add) || typeid(p) == typeid(Divide) ||
      typeid(p) == typeid(Equal) || typeid(p) == typeid(Greater) ||
      typeid(p) == typeid(GreaterEqual) || typeid(p) == typeid(Less) ||
      typeid(p) == typeid(LessEqual) || typeid(p) == typeid(LogicalNot) ||
      typeid(p) == typeid(LogicalAnd) || typeid(p) == typeid(LogicalOr) ||
      typeid(p) == typeid(LogAddExp) || typeid(p) == typeid(Maximum) ||
      typeid(p) == typeid(Minimum) || typeid(p) == typeid(Multiply) ||
      typeid(p) == typeid(NotEqual) || typeid(p) == typeid(Power) ||
      typeid(p) == typeid(Subtract));
}

bool is_broadcast(const Primitive& p) {
  return typeid(p) == typeid(Broadcast);
}

bool is_fusable(const Primitive& p) {
  return is_unary(p) || is_binary(p) || is_broadcast(p);
}

std::pair<std::vector<array>, std::vector<array>> convert_trace_to_real(
    const std::vector<array>& inputs,
    const std::vector<array>& trace_tape,
    const std::vector<array>& trace_inputs,
    const std::vector<array>& trace_outputs) {
  // TODO refactor this with compile_replace
  std::unordered_map<uintptr_t, array> trace_to_real;
  for (int i = 0; i < trace_inputs.size(); ++i) {
    trace_to_real.insert({trace_inputs[i].id(), inputs[i]});
  }
  std::vector<array> tape;
  for (auto& a : trace_tape) {
    // Arrays in the tape without primitives are constants
    // and can be used directly
    if (!a.has_primitive()) {
      trace_to_real.insert({a.id(), a});
      tape.push_back(a);
    } else {
      // Find real inputs
      std::vector<array> real_inputs;
      for (auto& in : a.inputs()) {
        real_inputs.push_back(trace_to_real.at(in.id()));
      }
      if (a.siblings().empty()) {
        auto real_a = array(
            a.shape(), a.dtype(), a.primitive_ptr(), std::move(real_inputs));
        trace_to_real.insert({a.id(), std::move(real_a)});
        tape.push_back(real_a);
      } else {
        // Ensure the order is correct for multi-output primitives
        std::vector<std::vector<int>> shapes;
        std::vector<Dtype> types;
        auto trace_out = a.outputs();
        for (auto& o : trace_out) {
          shapes.push_back(o.shape());
          types.push_back(o.dtype());
        }
        auto real_out =
            array::make_arrays(shapes, types, a.primitive_ptr(), real_inputs);
        // TODO choose the array at the sam eposition
        tape.push_back(real_out[0]);
        for (int i = 0; i < trace_out.size(); ++i) {
          trace_to_real.insert({trace_out[i].id(), std::move(real_out[i])});
        }
      }
    }
  }

  std::vector<array> outputs;
  for (auto& o : trace_outputs) {
    outputs.push_back(trace_to_real.at(o.id()));
  }
  return {tape, outputs};
}

Compiled::Compiled(
    Stream stream,
    std::vector<array> inputs,
    std::vector<array> outputs,
    std::vector<array> tape)
    : Primitive(stream),
      inputs_(std::move(inputs)),
      outputs_(std::move(outputs)),
      tape_(std::move(tape)) {}

std::vector<array> Compiled::vjp(
    const std::vector<array>& primals,
    const std::vector<array>& cotangents,
    const std::vector<int>& argnums,
    const std::vector<array>& outputs) {
  throw std::invalid_argument("[Compiled::vjp] NYI");
}

std::vector<array> Compiled::jvp(
    const std::vector<array>& primals,
    const std::vector<array>& tangents,
    const std::vector<int>& argnums) {
  throw std::invalid_argument("[Compiled::jvp] NYI");
}

std::pair<std::vector<array>, std::vector<int>> Compiled::vmap(
    const std::vector<array>& inputs,
    const std::vector<int>& axes) {
  // Inputs are the real inputs
  auto [s_outputs, tape] =
      convert_trace_to_real(inputs, inputs_, tape_, outputs_);

  // The next part is the standard vmap code, should refactor
  std::unordered_map<std::uintptr_t, std::pair<array, int>> tmap;
  for (int i = 0; i < inputs.size(); ++i) {
    auto in = inputs_[i];
    // TODO this may need to be real inputs to real inputs
    tmap.insert({in.id(), {inputs[i], axes[i]}});
  }

  for (auto& a : tape) {
    std::vector<array> v_inputs;
    std::vector<int> v_axes;
    for (auto& in : a.inputs()) {
      auto it = tmap.find(in.id());
      auto& [v_in, v_ax] = it->second;
      v_inputs.push_back(v_in);
      v_axes.push_back(v_ax);
    }

    auto [v_outputs, v_out_axes] = a.primitive().vmap(v_inputs, v_axes);

    auto outputs = a.outputs();
    for (int i = 0; i < v_outputs.size(); ++i) {
      tmap.insert({outputs[i].id(), {v_outputs[i], v_out_axes[i]}});
    }
  }

  // Construct the real outputs from the non-vmapped outputs
  std::vector<array> outputs;
  std::vector<int> out_axes;
  for (auto& o : s_outputs) {
    auto it = tmap.find(o.id());
    auto& [out, ax] = it->second;
    outputs.push_back(out);
    out_axes.push_back(ax);
  }
  return {outputs, out_axes};
}

bool Compiled::is_equivalent(const Primitive& other) const {
  // TODO equivalent if the tapes of primitives are equivalent?
  return false;
}

void Compiled::print(std::ostream& os) {
  // TODO maybe print the compiled name here instead.
  for (auto& a : tape_) {
    a.primitive().print(os);
  }
}

namespace detail {

bool& compiler_disabled() {
  auto get_val = []() {
    if (const char* buff_str = std::getenv("MLX_DISABLE_COMPILE")) {
      return true;
    } else {
      return false;
    }
  };
  static bool compiler_disabled_ = get_val();
  return compiler_disabled_;
}

using CompileFn = std::function<std::vector<array>(const std::vector<array>&)>;
using ParentsMap =
    std::unordered_map<std::uintptr_t, std::vector<std::pair<array, int>>>;

template <typename T, typename... U>
size_t getAddress(std::function<T(U...)> f) {
  typedef T(fnType)(U...);
  fnType** fnPointer = f.template target<fnType*>();
  if (fnPointer == nullptr) {
    throw std::invalid_argument(
        "[compile] Cannot compile a non-addressable function.");
  }
  return (size_t)*fnPointer;
}

struct CompilerCache {
  struct CacheEntry {
    std::vector<array> inputs;
    std::vector<array> outputs;
    std::vector<array> tape;
    bool empty{true};
  };

  // Returns a reference to a CacheEntry which can be updated
  // by the caller to avoid copying large tapes / inputs / outputs
  CacheEntry& find(size_t fun_id, const std::vector<array>& inputs) {
    // Try to find the entry
    auto [entry_it, inserted] = cache_.insert({fun_id, {}});
    auto& entries = entry_it->second;
    auto is_match = [](const std::vector<array>& in1,
                       const std::vector<array>& in2) {
      if (in1.size() != in2.size()) {
        throw std::runtime_error(
            "[compiler] Got different number of inputs to function,"
            " this should never happen.");
      }
      for (int i = 0; i < in1.size(); ++i) {
        if (in1[i].shape() != in2[i].shape()) {
          return false;
        }
        if (in1[i].dtype() != in2[i].dtype()) {
          return false;
        }
      }
      return true;
    };

    // Loop over entries and check inputs match i.e. shapes and types must be
    // equal. Note this could get really slow if one compiles the same
    // function with many different shapes. May want to store entries in a
    // more easily searchable structure.
    for (auto& entry : entries) {
      // Check the inputs match and return if so
      if (is_match(inputs, entry.inputs)) {
        return entry;
      }
    }
    // Otherwise append a new cache entry
    entries.push_back(CacheEntry{});
    return entries.back();
  };

  void erase(size_t fun_id) {
    cache_.erase(fun_id);
  }

 private:
  CompilerCache() {
    // Make sure the allocator is fully
    // initialized before the compiler cache
    allocator::allocator();
  }
  friend CompilerCache& compiler_cache();
  std::unordered_map<size_t, std::vector<CacheEntry>> cache_;
};

CompilerCache& compiler_cache() {
  static CompilerCache compiler_cache_;
  return compiler_cache_;
}

std::pair<std::vector<array>, std::vector<array>> compile_trace(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun,
    const std::vector<array>& inputs) {
  // Set the global tracing flag.
  detail::InTracing in_tracing;

  // Run the function on placeholder inputs
  // to get compute graph
  std::vector<array> tracer_inputs;
  for (int i = 0; i < inputs.size(); ++i) {
    array in(inputs[i].shape(), inputs[i].dtype(), nullptr, {});
    in.set_tracer(true);
    tracer_inputs.push_back(std::move(in));
  }
  return {tracer_inputs, fun(tracer_inputs)};
}

// Traverses the graph to build a tape and a map of array ids to their parents
std::pair<std::vector<array>, ParentsMap> compile_dfs(
    const std::vector<array>& inputs,
    const std::vector<array>& outputs) {
  std::function<void(const array&)> recurse;
  std::vector<array> tape;
  std::unordered_set<std::uintptr_t> input_set;
  std::unordered_map<std::uintptr_t, std::vector<std::pair<array, int>>>
      parents_map;
  for (int i = 0; i < inputs.size(); ++i) {
    auto in = inputs[i];
    input_set.insert(in.id());
  }

  // DFS the graph to build the tape, and log parents and scalars
  std::unordered_set<std::uintptr_t> cache;
  recurse = [&](const array& a) {
    auto id = a.id();
    if (cache.find(id) != cache.end()) {
      return;
    }
    for (int i = 0; i < a.inputs().size(); i++) {
      auto& in = a.inputs()[i];
      parents_map[in.id()].push_back({a, i});
      for (auto& s : a.siblings()) {
        parents_map[in.id()].push_back({s, i});
      }
      // Don't recurse on inputs (but add them to the tape for the purpose
      // of future optimizations)
      if (input_set.find(a.id()) == input_set.end()) {
        recurse(in);
      }
    }
    cache.insert(id);
    for (auto& s : a.siblings()) {
      cache.insert(s.id());
    }
    tape.push_back(a);
  };
  for (auto& a : outputs) {
    recurse(a);
  }
  return {tape, parents_map};
}

// Simplify the tape. Note, this function modifies in-place both the tape and
// the parents map to remove orphaned arrays
void compile_simplify(
    std::vector<array>& tape,
    ParentsMap& parents_map,
    const std::vector<array>& outputs,
    int passes) {
  // Helpers to identify identical scalars
  std::map<std::pair<uint64_t, Dtype::Val>, array> scalars;
  auto is_scalar = [](const array& a) {
    return a.is_evaled() && a.ndim() == 0;
  };
  auto get_scalar_rep = [](const array& a) {
    uint64_t v = 0;
    int dtype;
    switch (a.dtype().size) {
      case 1:
        v = *a.data<uint8_t>();
        break;
      case 4:
        v = *a.data<uint32_t>();
        break;
      case 8:
        v = *a.data<uint64_t>();
        break;
    }
    return std::make_pair(v, a.dtype().val);
  };

  for (auto& a : tape) {
    if (is_scalar(a)) {
      scalars.insert({get_scalar_rep(a), a});
    }
  }

  // Helper that fuses two arrays in the graph by setting the parents of the
  // source to point to the destination
  auto fuse = [&](array& dst, array& src) {
    // Canonicalize the order of the primitives outputs
    auto sources = src.outputs();
    auto dests = dst.outputs();
    // For each src parent, point it to the corresponding dest
    for (int i = 0; i < sources.size(); ++i) {
      auto src_parents = parents_map.find(sources[i].id());
      if (src_parents == parents_map.end()) {
        continue;
      }
      auto& pairs = parents_map[dests[i].id()];
      for (auto& parent : src_parents->second) {
        parent.first.inputs()[parent.second] = dests[i];
        pairs.push_back(parent);
      }
      // Remove the source from the map to avoid fusing with it again
      parents_map.erase(src_parents);
    }
  };

  // Depth-1 array equivalence check.
  auto array_equivalent = [](const array& a, const array& b) {
    if (!a.has_primitive() || !b.has_primitive()) {
      return false;
    }
    if (a.primitive_id() == b.primitive_id()) {
      return false;
    }
    const auto& pa = a.primitive();
    const auto& pb = b.primitive();
    if (typeid(pa) != typeid(pb)) {
      return false;
    }

    if (a.inputs().size() != b.inputs().size()) {
      return false;
    }

    for (int i = 0; i < a.inputs().size(); i++) {
      if (a.inputs()[i].id() != b.inputs()[i].id()) {
        return false;
      }
    }

    return pa.is_equivalent(pb);
  };

  // Pass 0: fuse scalars
  std::vector<array> new_tape;
  for (auto& arr : tape) {
    // Check if we can fuse scalars
    if (is_scalar(arr)) {
      auto scalar = scalars.find(get_scalar_rep(arr));
      if (scalar->second.id() != arr.id()) {
        fuse(scalar->second, arr);
        // Don't keep orphaned scalars in the tape
        continue;
      }
    }
    new_tape.push_back(std::move(arr));
  }

  tape = std::move(new_tape);

  std::unordered_set<uintptr_t> output_set;
  for (auto& o : outputs) {
    output_set.insert(o.id());
  }
  // Pass 1..passes: fuse only keeping non-orphaned arrays in the tape
  for (int pass = 0; pass < passes; ++pass) {
    for (auto& arr : tape) {
      // Helper to check if we can fuse the parents of the
      // given array
      auto maybe_fuse_parents = [&](auto& a) {
        auto parents = parents_map.find(a.id());
        if (parents != parents_map.end()) {
          auto N = parents->second.size();
          std::vector<bool> mask(N, false);
          for (int i = 0; i < N; i++) {
            if (mask[i]) {
              continue;
            }
            for (int j = i + 1; j < N; j++) {
              if (mask[j]) {
                continue;
              }
              auto& src = parents->second[j].first;
              auto& dst = parents->second[i].first;
              if (src.id() != dst.id() && array_equivalent(src, dst)) {
                fuse(dst, src);
                mask[j] = true;
              }
            }
          }
          // Erase orphaned parents so we don't keep fusing with them
          for (int i = N - 1; i > 0; --i) {
            if (mask[i]) {
              parents->second.erase(parents->second.begin() + i);
            }
          }
          return false;
        } else {
          return output_set.find(a.id()) == output_set.end();
        }
      };

      bool discard = maybe_fuse_parents(arr);
      for (auto& s : arr.siblings()) {
        discard &= maybe_fuse_parents(s);
      }
      // If an array and its siblings have no parents, and none of them are
      // outputs, it is safe to remove it from the tape
      if (!discard) {
        new_tape.push_back(std::move(arr));
      }
    }
    tape = std::move(new_tape);
  }
}

// Extract sub-graphs of the graph that can be compiled
// and replace them with a Compiled Primitive.
void compile_reduce(
    std::vector<array>& tape,
    ParentsMap& parents_map,
    const std::vector<array>& outputs) {
  std::vector<array> new_tape;
  std::unordered_set<uintptr_t> cache;
  std::vector<array> fused_inputs;
  std::vector<array> fused_outputs;

  // Go through the tape in reverse order
  // Keep iterating backward until either of:
  // - Max compile depth
  // - Reach an array with a parent outside the current section
  // - Reach an array with a primitve that we cannot compile
  for (int i = tape.size() - 1; i >= 0; --i) {
    cache.clear();
    fused_inputs.clear();
    fused_outputs.clear();

    auto s = i;
    while (s >= 0) {
      // Constant
      auto& in = tape[s];
      if (!in.has_primitive()) {
        cache.insert(in.id());
        s--;
        continue;
      }
      if (!is_fusable(in.primitive())) {
        break;
      }
      auto p_it = parents_map.find(in.id());

      // If no parents
      if (p_it == parents_map.end()) {
        fused_outputs.push_back(in);
        cache.insert(in.id());
        s--;
        continue;
      }
      auto& parents = p_it->second;
      if (parents.size() == 0) {
        throw std::runtime_error(
            "[compile_reduce] Why are you in the map without parents?");
      }
      bool all_parents_out = true;
      bool all_parents_in = true;
      bool continue_fusing = true;
      for (auto& [p, idx] : parents) {
        // Stop fusion, as parent is external to this section
        // of the tape
        auto in_cache = cache.find(p.id()) != cache.end();
        all_parents_in &= in_cache;
        all_parents_out &= !in_cache;
        if (!(all_parents_in || all_parents_out)) {
          continue_fusing = false;
          break;
        }
      }

      if (!continue_fusing) {
        break;
      }
      // If all parents are outside its an output
      fused_outputs.push_back(in);
      // If all parents are inside its an input
      fused_inputs.push_back(in);

      // Store in cache
      cache.insert(in.id());

      s--;
    }
    // No change needed if no fusion happened
    if (s == i) {
      continue;
    }

    std::vector<array> fused_tape(tape.begin() + s, tape.begin() + i + 1);
    std::vector<std::vector<int>> shapes;
    std::vector<Dtype> types;
    for (auto& o : fused_outputs) {
      shapes.push_back(o.shape());
      types.push_back(o.dtype());
    }
    auto compiled_outputs = array::make_arrays(
        shapes,
        types,
        std::make_shared<Compiled>(
            outputs.back().primitive().stream(),
            fused_inputs,
            std::move(fused_outputs),
            std::move(fused_tape)),
        fused_inputs);
    // One output per primitive
    new_tape.push_back(compiled_outputs[0]);
    new_tape.insert(new_tape.end(), fused_inputs.rbegin(), fused_inputs.rend());
  }

  std::reverse(new_tape.begin(), new_tape.end());
  tape = std::move(new_tape);
  // TODO, handle mismatched streams
  // TODO, maybe something special for siblings?
}

std::vector<array> compile_replace(
    const std::vector<array>& tape,
    const std::vector<array>& trace_inputs,
    const std::vector<array>& trace_outputs,
    const std::vector<array>& inputs) {
  std::unordered_map<uintptr_t, array> trace_to_real;
  for (int i = 0; i < inputs.size(); ++i) {
    trace_to_real.insert({trace_inputs[i].id(), inputs[i]});
  }

  for (auto& a : tape) {
    // Arrays in the tape without primitives are constants
    // and can be used directly
    if (!a.has_primitive()) {
      trace_to_real.insert({a.id(), a});
    } else {
      // Find real inputs
      std::vector<array> real_inputs;
      for (auto& in : a.inputs()) {
        real_inputs.push_back(trace_to_real.at(in.id()));
      }
      if (a.siblings().empty()) {
        auto real_a = array(
            a.shape(), a.dtype(), a.primitive_ptr(), std::move(real_inputs));
        trace_to_real.insert({a.id(), std::move(real_a)});
      } else {
        // Ensure the order is correct for multi-output primitives
        std::vector<std::vector<int>> shapes;
        std::vector<Dtype> types;
        auto trace_out = a.outputs();
        for (auto& o : trace_out) {
          shapes.push_back(o.shape());
          types.push_back(o.dtype());
        }
        auto real_out =
            array::make_arrays(shapes, types, a.primitive_ptr(), real_inputs);
        for (int i = 0; i < trace_out.size(); ++i) {
          trace_to_real.insert({trace_out[i].id(), std::move(real_out[i])});
        }
      }
    }
  }

  std::vector<array> outputs;
  for (auto& o : trace_outputs) {
    outputs.push_back(trace_to_real.at(o.id()));
  }
  return outputs;
}

std::function<std::vector<array>(const std::vector<array>&)> compile(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun,
    size_t fun_id) {
  if (compiler_disabled()) {
    return fun;
  }
  return [fun, fun_id](const std::vector<array>& inputs) {
    // Find a cache entry with the correct inputs
    auto& entry = compiler_cache().find(fun_id, inputs);

    // No matching cache entry existed, so compile
    if (entry.empty) {
      // Mark the entry as not empty since we are about to fill it
      entry.empty = false;
      // Trace to build the graph
      std::tie(entry.inputs, entry.outputs) = compile_trace(fun, inputs);

      // DFS the graph and get a tape, and a map of array id to (parent,
      // position in parent inputs)
      std::unordered_map<uintptr_t, std::vector<std::pair<array, int>>>
          parents_map;
      std::tie(entry.tape, parents_map) =
          compile_dfs(entry.inputs, entry.outputs);

      // Simplify the tape
      compile_simplify(entry.tape, parents_map, entry.outputs, /* passes */ 3);

      // This is a good point to do more optimizations, e.g. kernel fusion to
      // generate new primitives. The tape needs to be updated accordingly
    }

    // At this point we must have a tape, now replace the placeholders
    // with real arrays that can be evaluated
    return compile_replace(entry.tape, entry.inputs, entry.outputs, inputs);
  };
}

void compile_erase(size_t fun_id) {
  detail::compiler_cache().erase(fun_id);
}

} // namespace detail

std::function<std::vector<array>(const std::vector<array>&)> compile(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun) {
  if (detail::compiler_disabled()) {
    return fun;
  }
  auto fun_id = detail::getAddress(fun);
  return detail::compile(fun, fun_id);
}

void disable_compile() {
  detail::compiler_disabled() = true;
}

void enable_compile() {
  detail::compiler_disabled() = false;
}

} // namespace mlx::core
