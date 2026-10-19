#include <rex/cli_arg_list.h>

#include <algorithm>
#include <ranges>

namespace rex {

CLIArgList CLIArgList::Parse(i32 argc, char** argv) {
  std::vector<std::string> args;
  for (i32 index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  return Parse(args);
}

CLIArgList CLIArgList::Parse(std::span<const std::string> args) {
  CLIArgList list;
  list.all_.assign(args.begin(), args.end());
  for (size_t index = 0; index < args.size(); ++index) {
    const std::string& token = args[index];
    if (!token.starts_with("--")) {
      list.positionals_.push_back(token);
      continue;
    }
    std::string body = token.substr(2);
    if (const auto eq = body.find('='); eq != std::string::npos) {
      list.options_.push_back({body.substr(0, eq), body.substr(eq + 1)});
      continue;
    }
    const bool next_is_value = index + 1 < args.size() && !args[index + 1].starts_with("--");
    if (next_is_value) {
      list.options_.push_back({std::move(body), args[++index]});
    } else {
      list.options_.push_back({std::move(body), std::nullopt});
    }
  }
  return list;
}

bool CLIArgList::Has(std::string_view name) const {
  return std::ranges::any_of(options_,
                             [&](const CLIOption& option) { return option.name == name; });
}

std::optional<std::string_view> CLIArgList::Get(std::string_view name) const {
  for (const CLIOption& option : options_ | std::views::reverse) {
    if (option.name == name) {
      if (option.value) {
        return std::string_view(*option.value);
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::span<const CLIOption> CLIArgList::GetOptions() const {
  return options_;
}

std::span<const std::string> CLIArgList::GetPositionals() const {
  return positionals_;
}

std::span<const std::string> CLIArgList::GetAll() const {
  return all_;
}

}  // namespace rex
