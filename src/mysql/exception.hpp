// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2018, LH_Mouse. All wrongs reserved.

#ifndef POSEIDON_MYSQL_EXCEPTION_HPP_
#define POSEIDON_MYSQL_EXCEPTION_HPP_

#include "../exception.hpp"

namespace Poseidon {
namespace MySql {

class Exception : public BasicException {
private:
	SharedNts m_schema;
	unsigned long m_code;

public:
	Exception(const char *file, std::size_t line, const char *func, SharedNts schema, unsigned long code, SharedNts message);
	~Exception() NOEXCEPT;

public:
	const char *get_schema() const NOEXCEPT {
		return m_schema.get();
	}
	unsigned long get_code() const NOEXCEPT {
		return m_code;
	}
};

}
}

#endif
