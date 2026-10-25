#pragma once

#include <string>
#include <utility>
#include <variant>

namespace luau_vm {

template <typename T>
class Result {
public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(std::string error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return data_.index() == 0;
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const T& value() const {
        return std::get<0>(data_);
    }

    [[nodiscard]] T& value() {
        return std::get<0>(data_);
    }

    [[nodiscard]] const std::string& error() const {
        return std::get<1>(data_);
    }

private:
    template <typename... Args>
    explicit Result(std::in_place_index_t<0> tag, Args&&... args)
        : data_(tag, std::forward<Args>(args)...) {}

    template <typename... Args>
    explicit Result(std::in_place_index_t<1> tag, Args&&... args)
        : data_(tag, std::forward<Args>(args)...) {}

    std::variant<T, std::string> data_;
};

} // namespace luau_vm
