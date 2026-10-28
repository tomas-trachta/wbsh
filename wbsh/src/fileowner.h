#pragma once

/**
 * @file fileowner.h
 * @brief The NTFS owner and group of a file, read and set the way chown,
 *        chgrp and install need them.
 */

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wbsh {

	struct Account {
		std::vector<unsigned char> sid;
		std::string name;
	};

	/// A name (`trach`, `DOMAIN\trach`, `Administrators`), a SID string
	/// (`S-1-5-32-544`) or a number. The number `id` reports for the current
	/// user (1000) is that user; any other number is a RID in the current
	/// user's domain, accepted only when an account has it.
	bool resolveAccount(const std::string& text, Account& account);

	/// The account the shell runs as.
	bool currentAccount(Account& account);

	bool sameAccount(const Account& a, const Account& b);

	struct Ownership {
		Account owner;
		Account group;
	};

	/// With @p follow a symbolic link or junction stands for its target.
	bool readOwnership(const std::filesystem::path& native, bool follow, Ownership& ownership,
	                   std::error_code& ec);

	struct OwnershipChange {
		const Account* owner = nullptr;
		const Account* group = nullptr;
		bool follow = true;
	};

	/// Sets the parts of @p change that are not null. Every refusal by
	/// Windows (no WRITE_OWNER right, an owner the token may not assign, a
	/// volume without ACLs) comes back as std::errc::operation_not_permitted.
	bool writeOwnership(const std::filesystem::path& native, const OwnershipChange& change,
	                    std::error_code& ec);

	/// Enables SeRestorePrivilege and SeTakeOwnershipPrivilege for its
	/// lifetime when the token holds them, so an elevated shell can give
	/// files away; restores the previous state when it ends.
	class OwnershipPrivileges {
	public:
		OwnershipPrivileges();
		~OwnershipPrivileges();
		OwnershipPrivileges(const OwnershipPrivileges&) = delete;
		OwnershipPrivileges& operator=(const OwnershipPrivileges&) = delete;

	private:
		void* token_ = nullptr;
		std::vector<unsigned char> previous_;
	};

}  // namespace wbsh
