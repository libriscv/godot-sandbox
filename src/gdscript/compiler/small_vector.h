#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gdscript {

// std::vector subset with N elements stored in the object. Trivially copyable
// only: grow, copy and move are one memcpy, destruction one free().
template <typename T, size_t N>
class SmallVector {
	static_assert(std::is_trivially_copyable<T>::value,
		"SmallVector copies with memcpy and runs no destructor");

public:
	SmallVector() : m_data(inline_storage()), m_size(0), m_capacity(N) {}

	SmallVector(std::initializer_list<T> values) : SmallVector() {
		reserve(values.size());
		for (const T& value : values) {
			m_data[m_size++] = value;
		}
	}

	SmallVector(const SmallVector& other) : SmallVector() {
		assign(other.m_data, other.m_size);
	}

	SmallVector(SmallVector&& other) noexcept : SmallVector() {
		steal(other);
	}

	SmallVector& operator=(const SmallVector& other) {
		if (this != &other) {
			assign(other.m_data, other.m_size);
		}
		return *this;
	}

	SmallVector& operator=(SmallVector&& other) noexcept {
		if (this != &other) {
			release();
			m_data = inline_storage();
			m_capacity = N;
			steal(other);
		}
		return *this;
	}

	~SmallVector() { release(); }

	size_t size() const { return m_size; }
	bool empty() const { return m_size == 0; }

	T* data() { return m_data; }
	const T* data() const { return m_data; }
	T* begin() { return m_data; }
	T* end() { return m_data + m_size; }
	const T* begin() const { return m_data; }
	const T* end() const { return m_data + m_size; }

	T& operator[](size_t index) { return m_data[index]; }
	const T& operator[](size_t index) const { return m_data[index]; }

	T& at(size_t index) { check(index); return m_data[index]; }
	const T& at(size_t index) const { check(index); return m_data[index]; }

	T& back() { return m_data[m_size - 1]; }
	const T& back() const { return m_data[m_size - 1]; }

	void push_back(const T& value) {
		if (m_size == m_capacity) {
			reserve(m_size + 1);
		}
		m_data[m_size++] = value;
	}

	template <typename... Args>
	T& emplace_back(Args&&... args) {
		if (m_size == m_capacity) {
			reserve(m_size + 1);
		}
		m_data[m_size] = T(std::forward<Args>(args)...);
		return m_data[m_size++];
	}

	void pop_back() { m_size--; }
	void clear() { m_size = 0; }

	void resize(size_t count) {
		reserve(count);
		for (size_t i = m_size; i < count; i++) {
			m_data[i] = T();
		}
		m_size = uint32_t(count);
	}

	void reserve(size_t wanted) {
		if (wanted <= m_capacity) {
			return;
		}
		size_t capacity = m_capacity * 2;
		while (capacity < wanted) {
			capacity *= 2;
		}
		T* fresh = static_cast<T*>(std::malloc(capacity * sizeof(T)));
		if (fresh == nullptr) {
			throw std::bad_alloc();
		}
		std::memcpy(fresh, m_data, m_size * sizeof(T));
		release();
		m_data = fresh;
		m_capacity = uint32_t(capacity);
	}

private:
	T* inline_storage() { return reinterpret_cast<T*>(m_inline); }

	void check(size_t index) const {
		if (index >= m_size) {
			throw std::out_of_range("SmallVector index out of range");
		}
	}

	void release() {
		if (m_data != reinterpret_cast<const T*>(m_inline)) {
			std::free(m_data);
		}
	}

	void assign(const T* source, size_t count) {
		reserve(count);
		std::memcpy(m_data, source, count * sizeof(T));
		m_size = uint32_t(count);
	}

	void steal(SmallVector& other) {
		if (other.m_data == other.inline_storage()) {
			std::memcpy(m_data, other.m_data, other.m_size * sizeof(T));
		} else {
			m_data = other.m_data;
			m_capacity = other.m_capacity;
			other.m_data = other.inline_storage();
			other.m_capacity = N;
		}
		m_size = other.m_size;
		other.m_size = 0;
	}

	alignas(T) unsigned char m_inline[N * sizeof(T)];
	T* m_data;
	uint32_t m_size;
	uint32_t m_capacity;
};

} // namespace gdscript
