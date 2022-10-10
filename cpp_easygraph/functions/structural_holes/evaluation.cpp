#include "evaluation.h"

#include "../../classes/graph.h"
#include "../../common/utils.h"

struct pair_hash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>()(p.first);
        auto h2 = std::hash<T2>()(p.second);
        return h1 ^ h2;
    }
};

std::unordered_map<std::pair<node_t, node_t>, weight_t, pair_hash> sum_nmw_rec, max_nmw_rec, local_constraint_rec;

enum norm_t {
    sum,
    max
};

weight_t mutual_weight(Graph& G, node_t u, node_t v, std::string weight) {
    weight_t a_uv = 0, a_vu = 0;
    if (G.adj.count(u) && G.adj[u].count(v)) {
        edge_attr_dict_factory& guv = G.adj[u][v];
        a_uv = guv.count(weight) ? guv[weight] : 1;
    }
    if (G.adj.count(v) && G.adj[v].count(u)) {
        edge_attr_dict_factory& gvu = G.adj[v][u];
        a_vu = gvu.count(weight) ? gvu[weight] : 1;
    }
    return a_uv + a_vu;
}

weight_t normalized_mutual_weight(Graph& G, node_t u, node_t v, std::string weight, norm_t norm = sum) {
    std::pair<node_t, node_t> edge = std::make_pair(u, v);
    auto& nmw_rec = (norm == sum) ? sum_nmw_rec : max_nmw_rec;
    weight_t nmw;
    if (nmw_rec.count(edge)) {
        nmw = nmw_rec[edge];
    } else {
        weight_t scale = 0;
        for (auto& w : G.adj[u]) {
            weight_t temp_weight = mutual_weight(G, u, w.first, weight);
            scale = (norm == sum) ? (scale + temp_weight) : std::max(scale, temp_weight);
        }
        nmw = scale ? (mutual_weight(G, u, v, weight) / scale) : 0;
        nmw_rec[edge] = nmw;
    }
    return nmw;
}

weight_t local_constraint(Graph& G, node_t u, node_t v, std::string weight = "None") {
    std::pair<node_t, node_t> edge = std::make_pair(u, v);
    if (local_constraint_rec.count(edge)) {
        return local_constraint_rec[edge];
    } else {
        weight_t direct = normalized_mutual_weight(G, u, v, weight);
        weight_t indirect = 0;
        for (auto& w : G.adj[u]) {
            indirect += normalized_mutual_weight(G, u, w.first, weight) * normalized_mutual_weight(G, w.first, v, weight);
        }
        weight_t result = pow((direct + indirect), 2);
        local_constraint_rec[edge] = result;
        return result;
    }
}

std::pair<node_t, weight_t> compute_constraint_of_v(Graph& G, node_t v, std::string weight) {
    weight_t constraint_of_v = 0;
    if (G.adj[v].size() == 0) {
        constraint_of_v = Py_NAN;
    } else {
        for (const auto& n : G.adj[v]) {
            constraint_of_v += local_constraint(G, v, n.first, weight);
        }
    }
    return std::make_pair(v, constraint_of_v);
}

py::object constraint(py::object G, py::object nodes, py::object weight, py::object n_workers) {
    std::string weight_key = weight_to_string(weight);
    sum_nmw_rec.clear();
    max_nmw_rec.clear();
    local_constraint_rec.clear();
    if (nodes.is_none()) {
        nodes = G.attr("nodes");
    }
    py::list nodes_list = py::list(nodes);
    py::list constraint_results = py::list();
    Graph& G_ = G.cast<Graph&>();
    int nodes_list_len = py::len(nodes_list);
    for (int i = 0; i < nodes_list_len; i++) {
        py::object v = nodes_list[i];
        node_t v_id = G_.node_to_id[v].cast<node_t>();
        std::pair<node_t, weight_t> constraint_pair = compute_constraint_of_v(G_, v_id, weight_key);
        py::tuple constraint_of_v = py::make_tuple(G_.id_to_node[py::cast(constraint_pair.first)], constraint_pair.second);
        constraint_results.append(constraint_of_v);
    }
    py::dict constraint = py::dict(constraint_results);
    return constraint;
}

weight_t redundancy(Graph& G, node_t u, node_t v, std::string weight = "None") {
    weight_t r = 0;
    for (const auto& neighbor_info : G.adj[u]) {
        node_t w = neighbor_info.first;
        r += normalized_mutual_weight(G, u, w, weight) * normalized_mutual_weight(G, v, w, weight, max);
    }
    return 1 - r;
}

py::object effective_size(py::object G, py::object nodes, py::object weight, py::object n_workers) {
    Graph& G_ = G.cast<Graph&>();
    sum_nmw_rec.clear();
    max_nmw_rec.clear();
    py::dict effective_size = py::dict();
    if (nodes.is_none()) {
        nodes = G;
    }
    nodes = py::list(nodes);
    if (!G.attr("is_directed")().cast<bool>() && weight.is_none()) {
        int nodes_len = py::len(nodes);
        for (int i = 0; i < nodes_len; i++) {
            py::object v = nodes[py::cast(i)];
            if (py::len(G[v]) == 0) {
                effective_size[v] = py::cast(Py_NAN);
                continue;
            }
            py::object E = G.attr("ego_subgraph")(v);
            E.attr("remove_node")(v);
            weight_t size = E.attr("size")().cast<weight_t>();
            effective_size[v] = py::len(E) - (2 * size) / py::len(E);
        }
    } else {
        std::string weight_key = weight_to_string(weight);
        int nodes_len = py::len(nodes);
        for (int i = 0; i < nodes_len; i++) {
            py::object v = nodes[py::cast(i)];
            if (py::len(G[v]) == 0) {
                effective_size[v] = py::cast(Py_NAN);
                continue;
            }
            weight_t redundancy_sum = 0;
            node_t v_id = G_.node_to_id[v].cast<node_t>();
            for (const auto& neighbor_info : G_.adj[v_id]) {
                node_t u_id = neighbor_info.first;
                redundancy_sum += redundancy(G_, v_id, u_id, weight_key);
            }
            effective_size[v] = redundancy_sum;
        }
    }
    return effective_size;
}

py::object hierarchy(py::object G, py::object nodes, py::object weight, py::object n_workers) {
    sum_nmw_rec.clear();
    max_nmw_rec.clear();
    local_constraint_rec.clear();
    std::string weight_key = weight_to_string(weight);
    if (nodes.is_none()) {
        nodes = G.attr("nodes");
    }
    py::list nodes_list = py::list(nodes);

    Graph& G_ = G.cast<Graph&>();
    py::dict hierarchy = py::dict();
    int nodes_list_len = py::len(nodes_list);
    for (int i = 0; i < nodes_list_len; i++) {
        py::object v = nodes_list[i];
        py::object E = G.attr("ego_subgraph")(v);

        int n = py::len(E) - 1;

        weight_t C = 0;
        std::map<node_t, weight_t> c;
        py::list neighbors_of_v = py::list(G.attr("neighbors")(v));
        int neighbors_of_v_len = py::len(neighbors_of_v);
        for (int j = 0; j < neighbors_of_v_len; j++) {
            py::object w = neighbors_of_v[j];
            node_t v_id = G_.node_to_id[v].cast<node_t>();
            node_t w_id = G_.node_to_id[w].cast<node_t>();
            C += local_constraint(G_, v_id, w_id, weight_key);
            c[w_id] = local_constraint(G_, v_id, w_id, weight_key);
        }
        if (n > 1) {
            weight_t hierarchy_sum = 0;
            int neighbors_of_v_len = py::len(neighbors_of_v);
            for (int k = 0; k < neighbors_of_v_len; k++) {
                py::object w = neighbors_of_v[k];
                node_t w_id = G_.node_to_id[w].cast<node_t>();
                hierarchy_sum += c[w_id] / C * n * log(c[w_id] / C * n) / (n * log(n));
            }
            hierarchy[v] = hierarchy_sum;
        }
        if (!hierarchy.contains(v)) {
            hierarchy[v] = 0;
        }
    }
    return hierarchy;
}