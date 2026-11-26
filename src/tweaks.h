#pragma once

#include <functional>
#include <string>
#include <vector>

namespace xyz {

using LogFn = std::function<void(const std::wstring&)>;

// A ordem precisa bater com as abas da interface.
enum class Category { Input = 0, Bloat = 1, Privacy = 2, Roblox = 3, Clean = 4 };

struct Tweak {
    std::wstring id;
    Category category = Category::Input;
    std::wstring title;
    std::wstring description;
    bool recommended = false;  // vem marcado por padrao
    bool revertible = false;   // da para desfazer
    std::function<bool(const LogFn&)> apply;
    std::function<bool(const LogFn&)> revert;  // pode ficar vazio
};

std::vector<Tweak> BuildTweaks();
bool CreateRestorePoint(const LogFn& log);
bool IsAdministrator();
std::wstring DataDir();

}  // namespace xyz
