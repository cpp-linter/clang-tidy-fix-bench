#include "../include/kaad/exceptions.hpp"

namespace kaad {

static inline std::string to_array_string(std::span<const int> array) {
    std::string str = "[";
    bool first = true;
    for (auto elem : array) {
        if (!first) {
            str += ", ";
        }
        first = false;
        str += std::to_string(elem);
    }
    str += "]";
    return str;
}

std::string make_graph_errmsg(
    const char *err_type, std::size_t graph_idx, const char *op_name,
    const char *msg,
    std::initializer_list<std::pair<const char *, std::span<const int>>> arrays,
    std::initializer_list<std::pair<const char *, int>> numbers) {

    std::string errmsg;
    const std::size_t max_msg_len = 128;
    errmsg.reserve(max_msg_len);

    errmsg += err_type;
    errmsg += " in node[";
    errmsg += std::to_string(graph_idx);
    errmsg += "] (";
    errmsg += op_name;
    errmsg += "), ";
    errmsg += msg;

    bool empty = arrays.size() > 0 && numbers.size() > 0;
    if (empty) {
        errmsg += " (";
    }

    bool first = true;
    for (auto pair : numbers) {
        if (!first) {
            errmsg += ", ";
        }
        first = false;
        errmsg += pair.first;
        errmsg += "=";
        errmsg += std::to_string(pair.second);
    }

    first = false;
    for (auto pair : arrays) {
        if (!first) {
            errmsg += ", ";
        }
        first = false;
        errmsg += pair.first;
        errmsg += "=";
        errmsg += to_array_string(pair.second);
    }

    if (empty) {
        errmsg += ")";
    }

    return errmsg;
}

} // namespace kaad
