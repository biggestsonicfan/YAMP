#pragma once

// The resolved-symbol map every host builds by pattern-scanning its module DLL.
//
// The map itself is generic, but the SET of symbols is per-DLL-family: the m2ftg modules and the
// VF5FS modules need completely different symbols, and each family's byte patterns live with that
// host. So this is a template over the host's symbol enum, and each host writes:
//
//     enum class ImportSymbol { ... };              // in the host's own namespace
//     using Imports = pxd::ImportsT<ImportSymbol>;
//     Imports BuildSymbolMap(void* dll);            // the host's own pattern table
//
// That keeps one implementation of the container while letting the enums and the BuildSymbolMap
// definitions stay separate — two hosts linked into the same YAMP.exe would otherwise collide.

#include <map>
#include <cassert>
#include <initializer_list>
#include <utility>

#include "../wil/common.h"

namespace pxd
{
		template<typename SymbolT>
		class ImportsT
		{
		public:
			ImportsT(std::initializer_list<std::pair<const SymbolT, void*>> symbols)
				: m_symbolMap(std::move(symbols))
			{
			}

			ImportsT(const ImportsT&) = delete;
			ImportsT(ImportsT&&) = default;

			void Add(SymbolT key, void* value)
			{
				m_symbolMap.emplace(key, value);
			}

			auto GetSymbol(SymbolT symbol) const
			{
				assert(m_symbolMap.count(symbol) == 1);
				return m_symbolMap.find(symbol)->second;
			}

			// For symbols that only exist in some of the module DLLs (see I960_FETCH_EXEC):
			// null when the pattern found nothing, instead of asserting.
			void* TryGetSymbol(SymbolT symbol) const
			{
				const auto it = m_symbolMap.find(symbol);
				return it != m_symbolMap.end() ? it->second : nullptr;
			}

			auto GetSymbolRange(SymbolT symbol) const
			{
				assert(m_symbolMap.count(symbol) != 0);
				auto range = m_symbolMap.equal_range(symbol);
				return wil::make_range(range.first, range.second);
			}

		private:
			std::multimap<SymbolT, void*> m_symbolMap;
		};
	}
