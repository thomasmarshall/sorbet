#include "packager/packager.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/synchronization/blocking_counter.h"
#include "ast/Helpers.h"
#include "ast/treemap/treemap.h"
#include "common/FileOps.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "common/concurrency/WorkerPool.h"
#include "common/sort/sort.h"
#include "common/strings/formatting.h"
#include "core/AutocorrectSuggestion.h"
#include "core/Unfreeze.h"
#include "core/errors/packager.h"
#include "core/packages/MangledName.h"
#include "core/packages/PackageInfo.h"
#include "rapidjson/writer.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <sys/stat.h>

using namespace std;

namespace sorbet::packager {
namespace {

constexpr string_view PACKAGE_FILE_NAME = "__package.rb"sv;

bool isTestNamespace(const core::NameRef ns) {
    return ns == core::packages::PackageDB::TEST_NAMESPACE;
}

bool visibilityApplies(const core::packages::VisibleTo &vt, absl::Span<const core::NameRef> name) {
    if (vt.visibleToType == core::packages::VisibleToType::Wildcard) {
        // a wildcard will match if it's a proper prefix of the package name
        return vt.packageName == name.subspan(0, vt.packageName.size());
    } else {
        // otherwise it needs to be the same
        return vt.packageName == name;
    }
}

string buildValidLayersStr(const core::GlobalState &gs) {
    auto validLayers = gs.packageDB().layers();
    ENFORCE(validLayers.size() > 0);
    if (validLayers.size() == 1) {
        return string(validLayers.front().shortName(gs));
    }
    string result = "";
    for (int i = 0; i < validLayers.size() - 1; i++) {
        if (validLayers.size() > 2) {
            result += core::ErrorColors::format("`{}`, ", validLayers[i].shortName(gs));
        } else {
            result += core::ErrorColors::format("`{}` ", validLayers[i].shortName(gs));
        }
    }
    result += core::ErrorColors::format("or `{}`", validLayers.back().shortName(gs));
    return result;
}

struct FullyQualifiedName {
    vector<core::NameRef> parts;
    core::Loc loc;

    FullyQualifiedName() = default;
    FullyQualifiedName(vector<core::NameRef> parts, core::Loc loc) : parts(parts), loc(loc) {}
    explicit FullyQualifiedName(const FullyQualifiedName &) = default;
    FullyQualifiedName(FullyQualifiedName &&) = default;
    FullyQualifiedName &operator=(const FullyQualifiedName &) = delete;
    FullyQualifiedName &operator=(FullyQualifiedName &&) = default;

    FullyQualifiedName withPrefix(core::NameRef prefix) const {
        vector<core::NameRef> prefixed(parts.size() + 1);
        prefixed[0] = prefix;
        std::copy(parts.begin(), parts.end(), prefixed.begin() + 1);
        ENFORCE(prefixed.size() == parts.size() + 1);
        return {move(prefixed), loc};
    }

    bool isSuffix(const FullyQualifiedName &prefix) const {
        if (prefix.parts.size() >= parts.size()) {
            return false;
        }

        return std::equal(prefix.parts.begin(), prefix.parts.end(), parts.begin());
    }
};

struct PackageName {
    core::LocOffsets loc;
    core::packages::MangledName mangledName;
    FullyQualifiedName fullName;
    FullyQualifiedName fullTestPkgName;

    // Pretty print the package's (user-observable) name (e.g. Foo::Bar)
    string toString(const core::GlobalState &gs) const {
        return absl::StrJoin(fullName.parts, "::", core::packages::NameFormatter(gs));
    }

    bool operator==(const PackageName &rhs) const {
        return mangledName == rhs.mangledName;
    }

    bool operator!=(const PackageName &rhs) const {
        return mangledName != rhs.mangledName;
    }
};

struct Import {
    PackageName name;
    core::packages::ImportType type;

    Import(PackageName &&name, core::packages::ImportType type) : name(std::move(name)), type(type) {}
};

struct Export {
    FullyQualifiedName fqn;

    explicit Export(FullyQualifiedName &&fqn) : fqn(move(fqn)) {}

    const vector<core::NameRef> &parts() const {
        return fqn.parts;
    }

    static bool lexCmp(const Export &a, const Export &b) {
        // Lex sort by name.
        return core::packages::PackageInfo::lexCmp(a.parts(), b.parts());
    }
};

// For a given vector of NameRefs, this represents the "next" vector that does not begin with its
// prefix (without actually constructing it). Consider the following sorted names:
//
// [A B]
// [A B C]
// [A B D E]
//    <<<< Position of LexNext([A B]) roughly equivalent to [A B <Infinity>]
// [X Y]
// [X Y Z]
class LexNext final {
    absl::Span<const core::NameRef> names;

public:
    LexNext(const vector<core::NameRef> &names) : names(names) {}

    bool operator<(absl::Span<const core::NameRef> rhs) const {
        // Lexicographic comparison:
        for (auto lhsIt = names.begin(), rhsIt = rhs.begin(); lhsIt != names.end() && rhsIt != rhs.end();
             ++lhsIt, ++rhsIt) {
            if (lhsIt->rawId() < rhsIt->rawId()) {
                return true;
            } else if (rhsIt->rawId() < lhsIt->rawId()) {
                return false;
            }
        }

        // This is where this implementation differs from `std::lexicographic_compare`: if one name is the prefix of
        // another they're considered equal, wheras `std::lexicographic_compare` would return `true` if the LHS
        // was shorter.
        return false;
    }

    bool operator<(const Export &e) const {
        return *this < e.parts();
    }
};

class PackageInfoImpl final : public core::packages::PackageInfo {
public:
    core::packages::MangledName mangledName() const {
        return name.mangledName;
    }

    absl::Span<const core::NameRef> fullName() const {
        return absl::MakeSpan(name.fullName.parts);
    }

    absl::Span<const std::string> pathPrefixes() const {
        return absl::MakeSpan(packagePathPrefixes);
    }

    core::Loc fullLoc() const {
        return loc;
    }

    core::Loc declLoc() const {
        return declLoc_;
    }

    bool exportAll() const {
        return exportAll_;
    }

    bool visibleToTests() const {
        return visibleToTests_;
    }

    PackageName name;

    // loc for the package definition. Full loc, from class to end keyword. Used for autocorrects.
    core::Loc loc;
    // loc for the package definition. Single line (just the class def). Used for error messages.
    core::Loc declLoc_;
    // The possible path prefixes associated with files in the package, including path separator at end.
    vector<std::string> packagePathPrefixes = {};
    // The names of each package imported by this package.
    vector<Import> importedPackageNames = {};
    // List of exported items that form the body of this package's public API.
    // These are copied into every package that imports this package.
    vector<Export> exports_ = {};

    // Whether this package should just export everything
    bool exportAll_ = false;

    // The other packages to which this package is visible. If this vector is empty, then it means
    // the package is fully public and can be imported by anything.
    //
    // The `VisibleToType` here represents whether to treat this line as a "wildcard". `Wildcard` means the
    // `visible_to` line allows this package to be imported not just by the referenced package name
    // but also any package name underneath it. `Normal` means the package can be imported
    // by the referenced package name but not any child packages (unless they have a separate
    // `visible_to` line of their own.)
    vector<pair<PackageName, core::packages::VisibleToType>> visibleTo_ = {};

    // Whether `visible_to` directives should be ignored for test code
    bool visibleToTests_ = false;

    optional<pair<core::packages::StrictDependenciesLevel, core::LocOffsets>> strictDependenciesLevel_ = nullopt;
    optional<pair<core::NameRef, core::LocOffsets>> layer_ = nullopt;

    optional<pair<core::packages::StrictDependenciesLevel, core::LocOffsets>> strictDependenciesLevel() const {
        return strictDependenciesLevel_;
    }

    optional<pair<core::NameRef, core::LocOffsets>> layer() const {
        return layer_;
    }

    // ID of the strongly-connected component that this package is in, according to its graph of import dependencies
    optional<int> sccID = nullopt;

    // PackageInfoImpl is the only implementation of PackageInfo
    const static PackageInfoImpl &from(const core::packages::PackageInfo &pkg) {
        ENFORCE(pkg.exists());
        return reinterpret_cast<const PackageInfoImpl &>(pkg); // TODO is there a more idiomatic way to do this?
    }

    static PackageInfoImpl &from(core::packages::PackageInfo &pkg) {
        ENFORCE(pkg.exists());
        return reinterpret_cast<PackageInfoImpl &>(pkg); // TODO is there a more idiomatic way to do this?
    }

    unique_ptr<PackageInfo> deepCopy() const {
        return make_unique<PackageInfoImpl>(*this);
    }

    bool ownsSymbol(const core::GlobalState &gs, core::SymbolRef symbol) const {
        auto file = symbol.loc(gs).file();
        auto &pkg = gs.packageDB().getPackageForFile(gs, file);
        return this->mangledName() == pkg.mangledName();
    }

    PackageInfoImpl(PackageName name, core::Loc loc, core::Loc declLoc_) : name(name), loc(loc), declLoc_(declLoc_) {}
    explicit PackageInfoImpl(const PackageInfoImpl &) = default;
    PackageInfoImpl &operator=(const PackageInfoImpl &) = delete;

    // What order should these packages be in the import list?
    // Returns -1 if a should come before b, 0 if they are equivalent, and 1 if a should come after b.
    //
    // This method replicates the logic used at Stripe to order packages and thus has all of the associated "quirks".
    // In particular, the ordering in a given package is a function of the strictness level of the package, and it is
    // not a simple "false < layered < layered_dag < dag" ordering. The ordering is as follows:
    // For strictDependenciesLevel::False:
    // - layering violations
    // - imports to 'false' packages
    // - imports to 'layered' or stricter packages
    // - test imports
    // For strictDependenciesLevel::Layered and LayeredDag:
    // - layering violations
    // - imports to 'false' packages
    // - imports to 'layered' or 'layered_dag' packages
    // - imports to 'dag' packages
    // - test imports
    // For strictDependenciesLevel::Dag:
    // - layering violations
    // - imports to 'false', 'layered', or 'layered_dag' packages
    // - imports to 'dag' packages
    // - test imports
    // TODO(neil): explain the rationale behind this ordering (ie. why is not the simple "false < layered < layered_dag
    // < dag" ordering)
    int orderImports(const core::GlobalState &gs, const PackageInfo &a, bool aIsTestImport, const PackageInfo &b,
                     bool bIsTestImport) const {
        // Test imports always come last, and aren't sorted by `strict_dependencies`
        if (aIsTestImport && bIsTestImport) {
            return orderByAlphabetical(gs, a, b);
        } else if (aIsTestImport && !bIsTestImport) {
            return 1;
        } else if (!aIsTestImport && bIsTestImport) {
            return -1;
        } // Neither is a test import

        auto strictnessCompareResult = orderByStrictness(gs.packageDB(), a, b);
        if (strictnessCompareResult == 0) {
            return orderByAlphabetical(gs, a, b);
        }
        return strictnessCompareResult;
    }

    int orderByStrictness(const core::packages::PackageDB &packageDB, const PackageInfo &a,
                          const PackageInfo &b) const {
        if (!packageDB.enforceLayering() || !strictDependenciesLevel().has_value() ||
            !a.strictDependenciesLevel().has_value() || !b.strictDependenciesLevel().has_value() ||
            !a.layer().has_value() || !b.layer().has_value()) {
            return 0;
        }

        // Layering violations always come first
        auto aCausesLayeringViolation = causesLayeringViolation(packageDB, a.layer().value().first);
        auto bCausesLayeringViolation = causesLayeringViolation(packageDB, b.layer().value().first);
        if (aCausesLayeringViolation && bCausesLayeringViolation) {
            return 0;
        } else if (aCausesLayeringViolation && !bCausesLayeringViolation) {
            return -1;
        } else if (!aCausesLayeringViolation && bCausesLayeringViolation) {
            return 1;
        }

        auto aStrictDependenciesLevel = a.strictDependenciesLevel().value().first;
        auto bStrictDependenciesLevel = b.strictDependenciesLevel().value().first;
        switch (strictDependenciesLevel().value().first) {
            case core::packages::StrictDependenciesLevel::False: {
                // Sort order: Layering violations, false, layered or stricter
                switch (aStrictDependenciesLevel) {
                    case core::packages::StrictDependenciesLevel::False:
                        return bStrictDependenciesLevel == core::packages::StrictDependenciesLevel::False ? 0 : -1;
                    case core::packages::StrictDependenciesLevel::Layered:
                    case core::packages::StrictDependenciesLevel::LayeredDag:
                    case core::packages::StrictDependenciesLevel::Dag:
                        return bStrictDependenciesLevel == core::packages::StrictDependenciesLevel::False ? 1 : 0;
                }
            }
            case core::packages::StrictDependenciesLevel::Layered:
            case core::packages::StrictDependenciesLevel::LayeredDag: {
                // Sort order: Layering violations, false, layered or layered_dag, dag
                switch (aStrictDependenciesLevel) {
                    case core::packages::StrictDependenciesLevel::False:
                        return bStrictDependenciesLevel == core::packages::StrictDependenciesLevel::False ? 0 : -1;
                    case core::packages::StrictDependenciesLevel::Layered:
                    case core::packages::StrictDependenciesLevel::LayeredDag:
                        switch (bStrictDependenciesLevel) {
                            case core::packages::StrictDependenciesLevel::False:
                                return 1;
                            case core::packages::StrictDependenciesLevel::Layered:
                            case core::packages::StrictDependenciesLevel::LayeredDag:
                                return 0;
                            case core::packages::StrictDependenciesLevel::Dag:
                                return -1;
                        }
                    case core::packages::StrictDependenciesLevel::Dag:
                        return bStrictDependenciesLevel == core::packages::StrictDependenciesLevel::Dag ? 0 : 1;
                }
            }
            case core::packages::StrictDependenciesLevel::Dag: {
                // Sort order: Layering violations, false or layered or layered_dag, dag
                switch (aStrictDependenciesLevel) {
                    case core::packages::StrictDependenciesLevel::False:
                    case core::packages::StrictDependenciesLevel::Layered:
                    case core::packages::StrictDependenciesLevel::LayeredDag:
                        return bStrictDependenciesLevel == core::packages::StrictDependenciesLevel::Dag ? -1 : 0;
                    case core::packages::StrictDependenciesLevel::Dag:
                        return bStrictDependenciesLevel == core::packages::StrictDependenciesLevel::Dag ? 0 : 1;
                }
            }
        }
    }

    int orderByAlphabetical(const core::GlobalState &gs, const PackageInfo &a, const PackageInfo &b) const {
        auto aStrName = a.show(gs);
        auto bStrName = b.show(gs);
        if (aStrName == bStrName) {
            return 0;
        }
        return aStrName < bStrName ? -1 : 1;
    }

    optional<core::AutocorrectSuggestion> addImport(const core::GlobalState &gs, const PackageInfo &pkg,
                                                    bool isTestImport) const {
        auto &info = PackageInfoImpl::from(pkg);
        auto insertionLoc = core::Loc::none(loc.file());
        optional<core::AutocorrectSuggestion::Edit> deleteTestImportEdit = nullopt;
        if (!importedPackageNames.empty()) {
            packager::PackageName const *importToInsertAfter = nullptr;
            for (auto &import : importedPackageNames) {
                if (import.name == info.name) {
                    if (!isTestImport && import.type == core::packages::ImportType::Test) {
                        // There's already a test import for this package, so we'll convert it to a regular import.
                        // importToInsertAfter already tracks where we need to insert the import.
                        // So we can craft an edit to delete the `test_import` line, and then use the regular logic for
                        // adding an import to insert the `import`.
                        auto importLoc = core::Loc(fullLoc().file(), import.name.loc);
                        auto [lineStart, numWhitespace] = importLoc.findStartOfIndentation(gs);
                        auto beginPos =
                            lineStart.adjust(gs, -numWhitespace, 0).beginPos(); // -numWhitespace for the indentation
                        auto endPos = importLoc.endPos();
                        core::Loc replaceLoc(importLoc.file(), beginPos, endPos);
                        deleteTestImportEdit = {replaceLoc, ""};
                    } else {
                        // we already import this, and if so, don't return an autocorrect
                        return nullopt;
                    }
                }

                auto &importInfo = gs.packageDB().getPackageInfo(import.name.mangledName);
                if (!importInfo.exists()) {
                    importToInsertAfter = &import.name;
                    continue;
                }

                auto compareResult =
                    orderImports(gs, info, isTestImport, importInfo, import.type == core::packages::ImportType::Test);
                if (compareResult == 1 || compareResult == 0) {
                    importToInsertAfter = &import.name;
                }
            }
            if (!importToInsertAfter) {
                // Insert before the first import
                core::Loc beforePackageName = {loc.file(), importedPackageNames.front().name.loc};
                auto [beforeImport, numWhitespace] = beforePackageName.findStartOfIndentation(gs);
                auto endOfPrevLine = beforeImport.adjust(gs, -numWhitespace - 1, 0);
                insertionLoc = endOfPrevLine.copyWithZeroLength();
            } else {
                insertionLoc = core::Loc{loc.file(), importToInsertAfter->loc.copyEndWithZeroLength()};
            }
        } else {
            // if we don't have any imports, then we can try adding it
            // either before the first export, or if we have no
            // exports, then right before the final `end`
            int64_t exportLoc;
            if (!exports_.empty()) {
                exportLoc = exports_.front().fqn.loc.beginPos() - "export "sv.size() - 1;
            } else {
                exportLoc = loc.endPos() - "end"sv.size() - 1;
            }

            std::string_view file_source = loc.file().data(gs).source();

            // Defensively guard against the first export loc or the package's loc being invalid.
            if (exportLoc <= 0 || exportLoc >= file_source.size()) {
                ENFORCE(false, "Failed to find a valid starting loc");
                return nullopt;
            }

            // we want to find the end of the last non-empty line, so
            // let's do something gross: walk backward until we find non-whitespace
            while (isspace(file_source[exportLoc])) {
                exportLoc--;
                // this shouldn't happen in a well-formatted
                // `__package.rb` file, but just to be safe
                if (exportLoc == 0) {
                    return nullopt;
                }
            }
            insertionLoc = core::Loc(loc.file(), exportLoc + 1, exportLoc + 1);
        }
        ENFORCE(insertionLoc.exists());

        auto packageToImport = info.name.toString(gs);
        auto suggestionTitle = fmt::format("Import `{}` in package `{}`", packageToImport, name.toString(gs));
        vector<core::AutocorrectSuggestion::Edit> edits = {
            {insertionLoc, fmt::format("\n  {} {}", isTestImport ? "test_import" : "import", packageToImport)}};
        if (deleteTestImportEdit.has_value()) {
            edits.push_back(deleteTestImportEdit.value());
            suggestionTitle = fmt::format("Convert `{}` to `{}`", "test_import", "import");
        }
        core::AutocorrectSuggestion suggestion(suggestionTitle, edits);
        return {suggestion};
    }

    optional<core::AutocorrectSuggestion> addExport(const core::GlobalState &gs,
                                                    const core::SymbolRef newExport) const {
        auto insertionLoc = core::Loc::none(loc.file());
        // first let's try adding it to the end of the imports.
        if (!exports_.empty()) {
            auto lastOffset = exports_.back().fqn.loc.offsets();
            insertionLoc = core::Loc{loc.file(), lastOffset.copyEndWithZeroLength()};
        } else {
            // if we don't have any imports, then we can try adding it
            // either before the first export, or if we have no
            // exports, then right before the final `end`
            uint32_t exportLoc = loc.endPos() - "end"sv.size() - 1;
            // we want to find the end of the last non-empty line, so
            // let's do something gross: walk backward until we find non-whitespace
            const auto &file_source = loc.file().data(gs).source();
            while (isspace(file_source[exportLoc])) {
                exportLoc--;
                // this shouldn't happen in a well-formatted
                // `__package.rb` file, but just to be safe
                if (exportLoc == 0) {
                    return nullopt;
                }
            }
            insertionLoc = {loc.file(), exportLoc + 1, exportLoc + 1};
        }
        ENFORCE(insertionLoc.exists());

        // now find the appropriate place for it, specifically by
        // finding the import that directly precedes it, if any
        auto strName = newExport.show(gs);
        core::AutocorrectSuggestion suggestion(fmt::format("Export `{}` in package `{}`", strName, name.toString(gs)),
                                               {{insertionLoc, fmt::format("\n  export {}", strName)}});
        return {suggestion};
    }

    vector<vector<core::NameRef>> exports() const {
        vector<vector<core::NameRef>> rv;
        for (auto &e : exports_) {
            rv.emplace_back(e.fqn.parts);
        }
        return rv;
    }
    vector<vector<core::NameRef>> imports() const {
        vector<vector<core::NameRef>> rv;
        for (auto &i : importedPackageNames) {
            if (i.type == core::packages::ImportType::Normal) {
                rv.emplace_back(i.name.fullName.parts);
            }
        }
        return rv;
    }
    vector<vector<core::NameRef>> testImports() const {
        vector<vector<core::NameRef>> rv;
        for (auto &i : importedPackageNames) {
            if (i.type == core::packages::ImportType::Test) {
                rv.emplace_back(i.name.fullName.parts);
            }
        }
        return rv;
    }
    vector<core::packages::VisibleTo> visibleTo() const {
        vector<core::packages::VisibleTo> rv;
        for (auto &v : visibleTo_) {
            rv.emplace_back(v.first.fullName.parts, v.second);
        }
        return rv;
    }

    std::optional<core::packages::ImportType> importsPackage(core::packages::MangledName mangledName) const {
        if (!mangledName.exists()) {
            return std::nullopt;
        }

        auto imp =
            absl::c_find_if(importedPackageNames, [mangledName](auto &i) { return i.name.mangledName == mangledName; });
        if (imp == importedPackageNames.end()) {
            return nullopt;
        }

        return imp->type;
    }

    // Is it a layering violation to import otherPkg from this package?
    bool causesLayeringViolation(const core::packages::PackageDB &packageDB, const PackageInfo &otherPkg) const {
        if (!otherPkg.layer().has_value()) {
            return false;
        }

        return causesLayeringViolation(packageDB, otherPkg.layer().value().first);
    }

    bool causesLayeringViolation(const core::packages::PackageDB &packageDB, core::NameRef otherPkgLayer) const {
        if (!layer().has_value()) {
            return false;
        }

        auto pkgLayer = layer().value().first;
        auto pkgLayerIndex = packageDB.layerIndex(pkgLayer);
        auto otherPkgLayerIndex = packageDB.layerIndex(otherPkgLayer);

        return pkgLayerIndex < otherPkgLayerIndex;
    }

    // What is the minimum strict dependencies level that this package's imports must have?
    core::packages::StrictDependenciesLevel minimumStrictDependenciesLevel() const {
        if (!strictDependenciesLevel().has_value()) {
            return core::packages::StrictDependenciesLevel::False;
        }

        switch (strictDependenciesLevel().value().first) {
            case core::packages::StrictDependenciesLevel::False:
                return core::packages::StrictDependenciesLevel::False;
            case core::packages::StrictDependenciesLevel::Layered:
            case core::packages::StrictDependenciesLevel::LayeredDag:
                return core::packages::StrictDependenciesLevel::Layered;
            case core::packages::StrictDependenciesLevel::Dag:
                return core::packages::StrictDependenciesLevel::Dag;
        }
    }

    bool importsTransitively(const core::GlobalState &gs, const core::packages::MangledName &otherPkg) const {
        UnorderedSet<core::packages::MangledName> seen;
        vector<core::packages::MangledName> toVisit;
        toVisit.push_back(mangledName());

        while (!toVisit.empty()) {
            auto current = toVisit.back();
            toVisit.pop_back();
            if (seen.contains(current)) {
                continue;
            }
            seen.insert(current);

            if (current == otherPkg) {
                return true;
            }

            auto &info = PackageInfoImpl::from(gs.packageDB().getPackageInfo(current));

            for (auto &import : info.importedPackageNames) {
                if (import.type == core::packages::ImportType::Test ||
                    !gs.packageDB().getPackageInfo(import.name.mangledName).exists()) {
                    continue;
                }

                toVisit.push_back(import.name.mangledName);
            }
        }
        return false;
    }
};

// If the __package.rb file itself is a test file, then the whole package is a test-only package.
// For example, `test/__package.rb` is a test-only package (e.g. Critic in Stripe's codebase).
bool isTestOnlyPackage(const core::GlobalState &gs, const PackageInfoImpl &pkg) {
    return pkg.loc.file().data(gs).isPackagedTest();
}

[[nodiscard]] bool validatePackageName(core::Context ctx, const ast::UnresolvedConstantLit *constLit) {
    bool valid = true;
    while (constLit != nullptr) {
        if (absl::StrContains(constLit->cnst.shortName(ctx), "_")) {
            // By forbidding package names to have an underscore, we can trivially convert between
            // mangled names and unmangled names by replacing `_` with `::`.
            //
            // Even with packages into the symbol table this restriction is useful, because we have
            // a lot of tooling that will create directory structures like Foo_Bar to store
            // generated files associated with package Foo::Bar
            if (auto e = ctx.beginError(constLit->loc, core::errors::Packager::InvalidPackageName)) {
                e.setHeader("Package names cannot contain an underscore");
                auto replacement = absl::StrReplaceAll(constLit->cnst.shortName(ctx), {{"_", ""}});
                auto nameLoc = constLit->loc;
                // cnst is the last characters in the constant literal
                nameLoc.beginLoc = nameLoc.endLoc - constLit->cnst.shortName(ctx).size();

                e.addAutocorrect(core::AutocorrectSuggestion{
                    fmt::format("Replace `{}` with `{}`", constLit->cnst.shortName(ctx), replacement),
                    {core::AutocorrectSuggestion::Edit{ctx.locAt(nameLoc), replacement}}});
            }
            valid = false;
        }
        constLit = ast::cast_tree<ast::UnresolvedConstantLit>(constLit->scope);
    }

    return valid;
}

FullyQualifiedName getFullyQualifiedName(core::Context ctx, const ast::UnresolvedConstantLit *constantLit) {
    FullyQualifiedName fqn;
    fqn.loc = ctx.locAt(constantLit->loc);
    while (constantLit != nullptr) {
        fqn.parts.emplace_back(constantLit->cnst);
        constantLit = ast::cast_tree<ast::UnresolvedConstantLit>(constantLit->scope);
    }
    reverse(fqn.parts.begin(), fqn.parts.end());
    ENFORCE(!fqn.parts.empty());
    return fqn;
}

// Gets the package name in `tree` if applicable.
PackageName getPackageName(core::Context ctx, const ast::UnresolvedConstantLit *constantLit) {
    ENFORCE(constantLit != nullptr);

    PackageName pName;
    pName.loc = constantLit->loc;
    pName.fullName = getFullyQualifiedName(ctx, constantLit);
    pName.fullTestPkgName = pName.fullName.withPrefix(core::packages::PackageDB::TEST_NAMESPACE);

    // pname.mangledName will be populated later, when we have a mutable GlobalState

    return pName;
}

// TODO(jez) Rename this to lookupMangledName, and make it take a const GlobalState
void populateMangledName(core::GlobalState &gs, PackageName &pName) {
    pName.mangledName = core::packages::MangledName::mangledNameFromParts(gs, pName.fullName.parts);
}

void mustContainPackageDef(core::Context ctx, core::LocOffsets loc) {
    // HACKFIX: Tolerate completely empty packages. LSP does not support the notion of a deleted file, and
    // instead replaces deleted files with the empty string. It should really mark files as Tombstones instead.
    if (!ctx.file.data(ctx).source().empty()) {
        if (auto e = ctx.beginError(loc, core::errors::Packager::InvalidPackageDefinition)) {
            e.setHeader("`{}` file must contain a package definition", "__package.rb");
            e.addErrorNote("Package definitions are class definitions like `{}`.\n"
                           "    For more information, see http://go/package-layout",
                           "class Foo::Bar < PackageSpec");
        }
    }
}

ast::ExpressionPtr prependName(ast::ExpressionPtr scope) {
    auto lastConstLit = ast::cast_tree<ast::UnresolvedConstantLit>(scope);
    ENFORCE(lastConstLit != nullptr);
    while (auto constLit = ast::cast_tree<ast::UnresolvedConstantLit>(lastConstLit->scope)) {
        lastConstLit = constLit;
    }
    lastConstLit->scope =
        ast::MK::Constant(lastConstLit->scope.loc().copyWithZeroLength(), core::Symbols::PackageSpecRegistry());
    return scope;
}

bool startsWithPackageSpecRegistry(const ast::UnresolvedConstantLit &cnst) {
    if (auto scope = ast::cast_tree<ast::ConstantLit>(cnst.scope)) {
        return scope->symbol() == core::Symbols::PackageSpecRegistry();
    } else if (auto scope = ast::cast_tree<ast::UnresolvedConstantLit>(cnst.scope)) {
        return startsWithPackageSpecRegistry(*scope);
    } else {
        return false;
    }
}

ast::ExpressionPtr prependRoot(ast::ExpressionPtr scope) {
    auto *lastConstLit = &ast::cast_tree_nonnull<ast::UnresolvedConstantLit>(scope);
    while (auto constLit = ast::cast_tree<ast::UnresolvedConstantLit>(lastConstLit->scope)) {
        lastConstLit = constLit;
    }
    auto loc = lastConstLit->scope.loc();
    lastConstLit->scope = ast::MK::Constant(loc, core::Symbols::root());
    return scope;
}

bool recursiveVerifyConstant(core::Context ctx, core::NameRef fun, const ast::ExpressionPtr &root,
                             const ast::ExpressionPtr &expr) {
    if (ast::isa_tree<ast::EmptyTree>(expr)) {
        return true;
    }

    auto target = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    if (target == nullptr) {
        if (auto e = ctx.beginError(root.loc(), core::errors::Packager::InvalidConfiguration)) {
            e.setHeader("Argument to `{}` must be a constant", fun.show(ctx));
        }
        return false;
    }

    return recursiveVerifyConstant(ctx, fun, root, target->scope);
}

const ast::UnresolvedConstantLit *verifyConstant(core::Context ctx, core::NameRef fun, const ast::ExpressionPtr &expr) {
    auto target = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    if (target == nullptr) {
        if (auto e = ctx.beginError(expr.loc(), core::errors::Packager::InvalidConfiguration)) {
            e.setHeader("Argument to `{}` must be a constant", fun.show(ctx));
        }
        return nullptr;
    }

    if (recursiveVerifyConstant(ctx, fun, expr, target->scope)) {
        return target;
    }

    return nullptr;
}

// Binary search to find a packages index in the global packages list
uint16_t findPackageIndex(core::Context ctx, const PackageInfoImpl &pkg) {
    auto packages = ctx.state.packageDB().packages();
    return std::lower_bound(packages.begin(), packages.end(), pkg.fullName(),
                            [ctx](auto pkgName, auto &curFileFullName) {
                                auto &pkg = ctx.state.packageDB().getPackageInfo(pkgName);
                                return core::packages::PackageInfo::lexCmp(pkg.fullName(), curFileFullName);
                            }) -
           packages.begin();
}

// Interface for traversing the tree of package namespaces.
// Compactly represent current position in this tree with begin/end offsets. This relies on
// lexicographic sorting of the packages vector. Push operations take advantage of binary search
// to be O(log(end - begin)).
//
// For example with names=[Foo, Bar]
// 0 Foo
// 1 Foo::Bar::Baz    <-- begin
// 2 Foo::Bar::Blub
// 3 Foo::Buzz        <-- end
// 4 Quuz::Bang
// 5 Yaz
// 6 <end of list>
class PackageNamespaces final {
    using Bound = pair<uint16_t, uint16_t>;

    const absl::Span<const core::packages::MangledName> packages; // Mangled names sorted lexicographically
    const PackageInfoImpl &filePkg;                               // Package for current file
    // Current bounds:
    uint16_t begin;
    uint16_t end;

    const bool isTestFile;
    const uint16_t filePkgIdx;

    // Count of pushes once we have narrowed down to one possible package:
    int skips = 0;

    vector<Bound> bounds;
    vector<core::NameRef> nameParts;
    vector<core::LocOffsets> namePartsLocs;
    vector<pair<core::packages::MangledName, uint16_t>> curPkg;
    core::NameRef foundTestNS = core::NameRef::noName();
    core::LocOffsets foundTestNSLoc;

    static constexpr uint16_t SKIP_BOUND_VAL = 0;

public:
    PackageNamespaces(core::Context ctx, const PackageInfoImpl &filePkg)
        : packages(ctx.state.packageDB().packages()), filePkg(filePkg), begin(0), end(packages.size()),
          isTestFile(ctx.file.data(ctx).isPackagedTest()), filePkgIdx(findPackageIndex(ctx, filePkg)) {
        ENFORCE(packages.size() < numeric_limits<uint16_t>::max());
    }

    int depth() const {
        ENFORCE(nameParts.size() == namePartsLocs.size());
        return nameParts.size();
    }

    const vector<pair<core::NameRef, core::LocOffsets>> currentConstantName() const {
        vector<pair<core::NameRef, core::LocOffsets>> res;

        if (foundTestNS.exists()) {
            res.emplace_back(foundTestNS, foundTestNSLoc);
        }

        ENFORCE(nameParts.size() == namePartsLocs.size());
        for (size_t i = 0; i < nameParts.size(); ++i) {
            res.emplace_back(nameParts[i], namePartsLocs[i]);
        }
        return res;
    }

    core::packages::MangledName packageForNamespace() const {
        if (curPkg.empty()) {
            return core::packages::MangledName();
        }
        return curPkg.back().first;
    }

    bool onPackagePath(core::Context ctx) {
        if (begin <= filePkgIdx && filePkgIdx < end) {
            return true;
        }
        if (!curPkg.empty() && curPkg.back().first == filePkg.mangledName()) {
            return true;
        }
        return false;
    }

    void pushName(core::Context ctx, core::NameRef name, core::LocOffsets loc) {
        if (skips > 0) {
            skips++;
            return;
        }
        bool boundsEmpty = bounds.empty();

        if (isTestFile && boundsEmpty && !foundTestNS.exists()) {
            if (isTestNamespace(name)) {
                foundTestNS = name;
                foundTestNSLoc = loc;
                return;
            } else if (!isTestOnlyPackage(ctx, filePkg)) {
                // In test-only packages, code can freely be inside the package's namespace, or the
                // package's test namespace (i.e., either Critic or Test::Critic).
                // Convention would say that the former is for test helpers and the latter is for
                // runnable tests, but there is nothing enforcing this convention in Sorbet.
                //
                // If this *not* a test-only package, set bounds such that begin == end, stopping
                // any subsequent search.
                bounds.emplace_back(begin, end);
                nameParts.emplace_back(name);
                namePartsLocs.emplace_back(loc);
                begin = end = 0;
                return;
            }
        }

        if (!boundsEmpty && end - begin == 1 && packages[begin] == filePkg.mangledName() &&
            nameParts.size() >= ctx.state.packageDB().getPackageInfo(packages[begin]).fullName().size()) {
            // We have descended into a package with no sub-packages. At this point it is safe to
            // skip tracking of deeper constants.
            curPkg.emplace_back(packages[begin], SKIP_BOUND_VAL);
            skips++;
            return;
        }

        bounds.emplace_back(begin, end);
        nameParts.emplace_back(name);
        namePartsLocs.emplace_back(loc);

        auto lb = std::lower_bound(packages.begin() + begin, packages.begin() + end, nameParts,
                                   [ctx](auto pkgNr, auto &nameParts) -> bool {
                                       return core::packages::PackageInfo::lexCmp(
                                           ctx.state.packageDB().getPackageInfo(pkgNr).fullName(), nameParts);
                                   });
        auto ub =
            std::upper_bound(lb, packages.begin() + end, LexNext(nameParts), [ctx](auto &next, auto pkgNr) -> bool {
                return next < ctx.state.packageDB().getPackageInfo(pkgNr).fullName();
            });

        begin = lb - packages.begin();
        end = ub - packages.begin();

        if (begin != end) {
            auto &pkgInfo = ctx.state.packageDB().getPackageInfo(*lb);
            ENFORCE(pkgInfo.exists());
            if (nameParts.size() == pkgInfo.fullName().size()) {
                curPkg.emplace_back(*lb, bounds.size());
            }
        }
    }

    void popName() {
        auto prevSkips = skips;
        if (skips > 0) {
            skips--;
            if (skips > 0) {
                return;
            }
        }

        if (isTestFile && bounds.size() == 0 && foundTestNS.exists()) {
            ENFORCE(nameParts.empty());
            foundTestNS = core::NameRef::noName();
            foundTestNSLoc = core::LocOffsets::none();
            return;
        }

        if (prevSkips == 1) {
            ENFORCE(curPkg.back().second == SKIP_BOUND_VAL);
            curPkg.pop_back();
            return;
        }

        if (begin != end && !curPkg.empty()) {
            ENFORCE(!curPkg.empty());
            auto back = curPkg.back();
            if (bounds.size() == back.second) {
                curPkg.pop_back();
            }
        }
        ENFORCE(!bounds.empty());
        begin = bounds.back().first;
        end = bounds.back().second;
        bounds.pop_back();
        nameParts.pop_back();
        namePartsLocs.pop_back();
    }

    ~PackageNamespaces() {
        // Book-keeping sanity checks
        ENFORCE(bounds.empty());
        ENFORCE(nameParts.empty());
        ENFORCE(namePartsLocs.empty());
        ENFORCE(begin == 0);
        ENFORCE(end = packages.size());
        ENFORCE(curPkg.empty());
        ENFORCE(!foundTestNS.exists());
        ENFORCE(skips == 0);
    }
};

// Visitor that ensures for constants defined within a package that all have the package as a
// prefix.
class EnforcePackagePrefix final {
    const PackageInfoImpl &pkg;

    // Whether code in this file must use the `Test::` namespace.
    //
    // Obviously tests *can* use the `Test::` namespace, but tests in test-only packages don't have to.
    //
    // (This is a wart of the original implementation, not an intentional design choice. It would
    // probably be good in the future to require that runnable tests live in the `Test::` namespace
    // for the package.)
    const bool mustUseTestNamespace;

    PackageNamespaces namespaces;
    // Counter to avoid duplicate errors:
    // - Only emit errors when depth is 0
    // - Upon emitting an error increment
    // - Once greater than 0, all preTransform* increment, postTransform* decrement
    int errorDepth = 0;
    int rootConsts = 0;
    bool useTestNamespace = false;
    vector<std::pair<core::NameRef, core::LocOffsets>> tmpNameParts;

public:
    EnforcePackagePrefix(core::Context ctx, const PackageInfoImpl &pkg)
        : pkg(pkg), mustUseTestNamespace(ctx.file.data(ctx).isPackagedTest() && !isTestOnlyPackage(ctx, pkg)),
          namespaces(ctx, pkg) {
        ENFORCE(pkg.exists());
    }

    void preTransformClassDef(core::Context ctx, const ast::ClassDef &classDef) {
        if (classDef.symbol == core::Symbols::root()) {
            // Ignore top-level <root>
            return;
        }
        if (errorDepth > 0) {
            errorDepth++;
            return;
        }

        auto constantLit = ast::cast_tree<ast::UnresolvedConstantLit>(classDef.name);
        if (constantLit == nullptr) {
            return;
        }

        pushConstantLit(ctx, constantLit);

        if (rootConsts == 0) {
            if (hasParentClass(classDef)) {
                // A class definition that includes a parent `class Foo::Bar < Baz`
                // must be made in that package
                checkBehaviorLoc(ctx, classDef.declLoc);
            } else if (!namespaces.onPackagePath(ctx)) {
                ENFORCE(errorDepth == 0);
                errorDepth++;
                if (auto e = ctx.beginError(constantLit->loc, core::errors::Packager::DefinitionPackageMismatch)) {
                    definitionPackageMismatch(ctx, e);
                }
            }
        }
    }

    void postTransformClassDef(core::Context ctx, const ast::ClassDef &classDef) {
        if (classDef.symbol == core::Symbols::root()) {
            // Sanity check bookkeeping
            ENFORCE(rootConsts == 0);
            ENFORCE(errorDepth == 0);
            return;
        }

        if (errorDepth > 0) {
            errorDepth--;
            // only continue if this was the first occurrence of the error
            if (errorDepth > 0) {
                return;
            }
        }

        auto constantLit = ast::cast_tree<ast::UnresolvedConstantLit>(classDef.name);
        if (constantLit == nullptr) {
            return;
        }

        popConstantLit(constantLit);
    }

    void preTransformAssign(core::Context ctx, const ast::Assign &asgn) {
        if (errorDepth > 0) {
            errorDepth++;
            return;
        }
        auto lhs = ast::cast_tree<ast::UnresolvedConstantLit>(asgn.lhs);

        if (lhs != nullptr && rootConsts == 0) {
            pushConstantLit(ctx, lhs);

            if (rootConsts == 0 && namespaces.packageForNamespace() != pkg.mangledName()) {
                ENFORCE(errorDepth == 0);
                errorDepth++;
                if (auto e = ctx.beginError(lhs->loc, core::errors::Packager::DefinitionPackageMismatch)) {
                    definitionPackageMismatch(ctx, e);
                }
            }

            popConstantLit(lhs);
        }
    }

    void postTransformAssign(core::Context ctx, const ast::Assign &asgn) {
        if (errorDepth > 0) {
            errorDepth--;
        }
    }

    void preTransformMethodDef(core::Context ctx, const ast::MethodDef &def) {
        if (errorDepth > 0) {
            errorDepth++;
            return;
        }
        checkBehaviorLoc(ctx, def.declLoc);
    }

    void postTransformMethodDef(core::Context ctx, const ast::MethodDef &def) {
        if (errorDepth > 0) {
            errorDepth--;
        }
    }

    void preTransformSend(core::Context ctx, const ast::Send &send) {
        if (errorDepth > 0) {
            errorDepth++;
            return;
        }
        checkBehaviorLoc(ctx, send.loc);
    }

    void postTransformSend(core::Context ctx, const ast::Send &send) {
        if (errorDepth > 0) {
            errorDepth--;
        }
    }

    void checkBehaviorLoc(core::Context ctx, core::LocOffsets loc) {
        ENFORCE(errorDepth == 0);
        if (rootConsts > 0 || namespaces.depth() == 0) {
            return;
        }
        auto &pkgName = requiredNamespace(ctx);
        auto packageForNamespace = namespaces.packageForNamespace();
        if (packageForNamespace != pkg.mangledName()) {
            ENFORCE(errorDepth == 0);
            errorDepth++;
            if (auto e = ctx.beginError(loc, core::errors::Packager::DefinitionPackageMismatch)) {
                e.setHeader("This file must only define behavior in enclosing package `{}`",
                            fmt::map_join(pkgName, "::", [&](const auto &nr) { return nr.show(ctx); }));
                const auto &constantName = namespaces.currentConstantName();
                e.addErrorLine(ctx.locAt(constantName.back().second), "Defining behavior in `{}` instead:",
                               fmt::map_join(constantName, "::", [&](const auto &nr) { return nr.first.show(ctx); }));
                e.addErrorLine(pkg.declLoc(), "Enclosing package `{}` declared here",
                               fmt::map_join(pkgName, "::", [&](const auto &nr) { return nr.show(ctx); }));
                if (packageForNamespace.exists()) {
                    auto &packageInfo = ctx.state.packageDB().getPackageInfo(packageForNamespace);
                    e.addErrorLine(packageInfo.declLoc(), "Package `{}` declared here",
                                   constantName.back().first.show(ctx));
                }
            }
        }
    }

private:
    void pushConstantLit(core::Context ctx, const ast::UnresolvedConstantLit *lit) {
        ENFORCE(tmpNameParts.empty());
        auto prevDepth = namespaces.depth();
        while (lit != nullptr) {
            tmpNameParts.emplace_back(lit->cnst, lit->loc);
            auto scope = ast::cast_tree<ast::ConstantLit>(lit->scope);
            lit = ast::cast_tree<ast::UnresolvedConstantLit>(lit->scope);
            if (scope != nullptr) {
                ENFORCE(lit == nullptr);
                ENFORCE(scope->symbol() == core::Symbols::root());
                rootConsts++;
            }
        }
        if (rootConsts == 0) {
            for (auto it = tmpNameParts.rbegin(); it != tmpNameParts.rend(); ++it) {
                namespaces.pushName(ctx, it->first, it->second);
            }
        }

        if (prevDepth == 0 && mustUseTestNamespace && namespaces.depth() > 0) {
            useTestNamespace = true;
        }

        tmpNameParts.clear();
    }

    void popConstantLit(const ast::UnresolvedConstantLit *lit) {
        while (lit != nullptr) {
            if (rootConsts == 0) {
                namespaces.popName();
            }
            auto scope = ast::cast_tree<ast::ConstantLit>(lit->scope);
            lit = ast::cast_tree<ast::UnresolvedConstantLit>(lit->scope);
            if (scope != nullptr) {
                ENFORCE(lit == nullptr);
                ENFORCE(scope->symbol() == core::Symbols::root());
                rootConsts--;
            }
        }
    }

    const vector<core::NameRef> &requiredNamespace(const core::GlobalState &gs) const {
        return useTestNamespace ? pkg.name.fullTestPkgName.parts : pkg.name.fullName.parts;
    }

    bool hasParentClass(const ast::ClassDef &def) const {
        return def.kind == ast::ClassDef::Kind::Class && !def.ancestors.empty() &&
               ast::isa_tree<ast::UnresolvedConstantLit>(def.ancestors[0]);
    }

    void definitionPackageMismatch(const core::GlobalState &gs, core::ErrorBuilder &e) const {
        auto requiredName =
            fmt::format("{}", fmt::map_join(requiredNamespace(gs), "::", [&](const auto nr) { return nr.show(gs); }));

        if (useTestNamespace) {
            e.setHeader("Tests in the `{}` package must define tests in the `{}` namespace", pkg.show(gs),
                        requiredName);
            // TODO: If the only thing missing is a `Test::` prefix (e.g., if this were not a test
            // file there would not have been an error), then we could suggest an autocorrect.
        } else {
            e.setHeader("File belongs to package `{}` but defines a constant that does not match this namespace",
                        requiredName);
        }

        e.addErrorLine(pkg.declLoc(), "Enclosing package declared here");

        auto reqMangledName = namespaces.packageForNamespace();
        if (reqMangledName.exists()) {
            auto &reqPkg = gs.packageDB().getPackageInfo(reqMangledName);
            auto givenNamespace =
                absl::StrJoin(namespaces.currentConstantName(), "::", core::packages::NameFormatter(gs));
            e.addErrorLine(reqPkg.declLoc(), "Must belong to this package, given constant name `{}`", givenNamespace);
        }
    }
};

struct PackageSpecBodyWalk {
    PackageSpecBodyWalk(PackageInfoImpl &info) : info(info) {}

    PackageInfoImpl &info;
    vector<Export> exported;
    bool foundFirstPackageSpec = false;
    bool foundLayerDeclaration = false;
    bool foundStrictDependenciesDeclaration = false;

    void postTransformSend(core::Context ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);

        // Disallowed methods
        if (send.fun == core::Names::extend() || send.fun == core::Names::include()) {
            if (auto e = ctx.beginError(send.loc, core::errors::Packager::InvalidPackageExpression)) {
                e.setHeader("Invalid expression in package: `{}` is not allowed", send.fun.shortName(ctx));
            }
            return;
        }

        // Sanity check arguments for unrecognized methods
        if (!isSpecMethod(send)) {
            for (auto &arg : send.posArgs()) {
                if (!ast::isa_tree<ast::Literal>(arg)) {
                    if (auto e = ctx.beginError(arg.loc(), core::errors::Packager::InvalidPackageExpression)) {
                        e.setHeader("Invalid expression in package: Arguments to functions must be literals");
                    }
                }
            }
        }

        if (send.fun == core::Names::export_() && send.numPosArgs() == 1) {
            // null indicates an invalid export.
            if (auto target = verifyConstant(ctx, core::Names::export_(), send.getPosArg(0))) {
                exported.emplace_back(getFullyQualifiedName(ctx, target));
                auto &arg = send.getPosArg(0);
                arg = prependRoot(std::move(arg));
            }
        }

        if ((send.fun == core::Names::import() || send.fun == core::Names::testImport()) && send.numPosArgs() == 1) {
            // null indicates an invalid import.
            if (auto *target = verifyConstant(ctx, send.fun, send.getPosArg(0))) {
                // Transform: `import Foo` -> `import <PackageSpecRegistry>::Foo`
                auto &posArg = send.getPosArg(0);
                auto importArg = move(posArg);
                posArg = prependName(move(importArg));

                info.importedPackageNames.emplace_back(getPackageName(ctx, target), method2ImportType(send));
            }
        }

        if (send.fun == core::Names::restrictToService() && send.numPosArgs() == 1) {
            // Transform: `restrict_to_service Foo` -> `restrict_to_service <PackageSpecRegistry>::Foo`
            auto &posArg = send.getPosArg(0);
            auto importArg = move(posArg);
            posArg = prependName(move(importArg));
        }

        if (send.fun == core::Names::exportAll() && send.numPosArgs() == 0) {
            info.exportAll_ = true;
        }

        if (send.fun == core::Names::visibleTo() && send.numPosArgs() == 1) {
            if (auto target = ast::cast_tree<ast::Literal>(send.getPosArg(0))) {
                // the only valid literal here is `visible_to "tests"`; others should be rejected
                if (!target->isString() || target->asString() != core::Names::tests()) {
                    if (auto e = ctx.beginError(target->loc, core::errors::Packager::InvalidConfiguration)) {
                        e.setHeader("Argument to `{}` must be a constant or the string literal `{}`",
                                    send.fun.show(ctx), "\"tests\"");
                    }
                    return;
                }
                info.visibleToTests_ = true;
            } else if (auto target = ast::cast_tree<ast::Send>(send.getPosArg(0))) {
                // Constant::* is valid Ruby, and parses as a send of the method * to Constant
                // so let's take advantage of this to implement wildcards
                if (target->fun != core::Names::star() || target->numPosArgs() > 0 || target->numKwArgs() > 0 ||
                    target->hasBlock()) {
                    if (auto e = ctx.beginError(target->loc, core::errors::Packager::InvalidConfiguration)) {
                        e.setHeader("Argument to `{}` must be a constant or the string literal `{}`",
                                    send.fun.show(ctx), "\"tests\"");
                    }
                    return;
                }

                if (auto *recv = verifyConstant(ctx, send.fun, target->recv)) {
                    auto &posArg = send.getPosArg(0);
                    auto importArg = move(target->recv);
                    posArg = prependName(move(importArg));
                    info.visibleTo_.emplace_back(getPackageName(ctx, recv), core::packages::VisibleToType::Wildcard);
                } else {
                    if (auto e = ctx.beginError(target->loc, core::errors::Packager::InvalidConfiguration)) {
                        e.setHeader("Argument to `{}` must be a constant or the string literal `{}`",
                                    send.fun.show(ctx), "\"tests\"");
                    }
                    return;
                }
            } else if (auto *target = verifyConstant(ctx, send.fun, send.getPosArg(0))) {
                auto &posArg = send.getPosArg(0);
                auto importArg = move(posArg);
                posArg = prependName(move(importArg));

                info.visibleTo_.emplace_back(getPackageName(ctx, target), core::packages::VisibleToType::Normal);
            }
        }

        if (send.fun == core::Names::strictDependencies()) {
            foundStrictDependenciesDeclaration = true;
            if (!ctx.state.packageDB().enforceLayering()) {
                if (auto e = ctx.beginError(send.loc, core::errors::Packager::InvalidStrictDependencies)) {
                    e.setHeader("Found `{}` annotation, but `{}` was not passed", send.fun.show(ctx),
                                "--packager-layers");
                    e.addErrorNote("Use `{}` to define the valid layers, or `{}` to use the default layers "
                                   "of `{}` and `{}`",
                                   "--packager-layers=foo,bar", "--packager-layers", "library", "application");
                }
                return;
            }
            if (info.strictDependenciesLevel_.has_value()) {
                if (auto e = ctx.beginError(send.loc, core::errors::Packager::InvalidStrictDependencies)) {
                    e.setHeader("Repeated declaration of `{}`", send.fun.show(ctx));
                    e.addErrorLine(ctx.locAt(info.strictDependenciesLevel_.value().second), "Previously declared here");
                    e.replaceWith("Remove this declaration", ctx.locAt(send.loc), "");
                }
                return;
            }

            if (send.numPosArgs() > 0) {
                auto parsedValue = parseStrictDependenciesOption(send.getPosArg(0));
                if (parsedValue.has_value()) {
                    info.strictDependenciesLevel_ = make_pair(parsedValue.value(), send.getPosArg(0).loc());
                } else {
                    if (auto e = ctx.beginError(send.argsLoc(), core::errors::Packager::InvalidStrictDependencies)) {
                        e.setHeader("Argument to `{}` must be one of: `{}`, `{}`, `{}`, or `{}`", send.fun.show(ctx),
                                    "'false'", "'layered'", "'layered_dag'", "'dag'");
                    }
                }
            }
        }

        if (send.fun == core::Names::layer()) {
            foundLayerDeclaration = true;
            if (!ctx.state.packageDB().enforceLayering()) {
                if (auto e = ctx.beginError(send.loc, core::errors::Packager::InvalidLayer)) {
                    e.setHeader("Found `{}` annotation, but `{}` was not passed", send.fun.show(ctx),
                                "--packager-layers");
                    e.addErrorNote("Use `{}` to define the valid layers, or `{}` to use the default layers "
                                   "of `{}` and `{}`",
                                   "--packager-layers=foo,bar", "--packager-layers", "library", "application");
                }
                return;
            }
            if (info.layer_.has_value()) {
                if (auto e = ctx.beginError(send.loc, core::errors::Packager::InvalidLayer)) {
                    e.setHeader("Repeated declaration of `{}`", send.fun.show(ctx));
                    e.addErrorLine(ctx.locAt(info.layer_.value().second), "Previously declared here");
                    e.replaceWith("Remove this declaration", ctx.locAt(send.loc), "");
                }
                return;
            }

            if (send.numPosArgs() > 0) {
                auto parsedValue = parseLayerOption(ctx.state, send.getPosArg(0));
                if (parsedValue.has_value()) {
                    info.layer_ = make_pair(parsedValue.value(), send.getPosArg(0).loc());
                } else {
                    if (auto e = ctx.beginError(send.argsLoc(), core::errors::Packager::InvalidLayer)) {
                        e.setHeader("Argument to `{}` must be one of: {}", send.fun.show(ctx),
                                    buildValidLayersStr(ctx.state));
                    }
                }
            }
        }
    }

    void preTransformClassDef(core::Context ctx, const ast::ExpressionPtr &tree) {
        auto &classDef = ast::cast_tree_nonnull<ast::ClassDef>(tree);
        if (classDef.symbol == core::Symbols::root()) {
            // Ignore top-level <root>
            return;
        }

        auto nameTree = ast::cast_tree<ast::UnresolvedConstantLit>(classDef.name);
        if (nameTree == nullptr) {
            // Already reported an error
            return;
        }

        if (startsWithPackageSpecRegistry(*nameTree)) {
            this->foundFirstPackageSpec = true;
        } else if (this->foundFirstPackageSpec) {
            if (auto e = ctx.beginError(classDef.declLoc, core::errors::Packager::MultiplePackagesInOneFile)) {
                e.setHeader("Package files can only declare one package");
                e.addErrorLine(info.loc, "Previous package declaration found here");
            }
        } else {
            mustContainPackageDef(ctx, tree.loc());
        }
    }

    // Generate a list of FQNs exported by this package. No export may be a prefix of another.
    void finalize(core::Context ctx) {
        if (exported.empty()) {
            return;
        }

        if (info.exportAll()) {
            // we're only here because exports exist, which means if
            // `exportAll` is set then we've got conflicting
            // information about export; flag the exports as wrong
            for (auto it = exported.begin(); it != exported.end(); ++it) {
                if (auto e = ctx.beginError(it->fqn.loc.offsets(), core::errors::Packager::ExportConflict)) {
                    e.setHeader("Package `{}` declares `{}` and therefore should not use explicit exports",
                                info.name.toString(ctx), "export_all!");
                }
            }
        }

        fast_sort(exported, Export::lexCmp);
        vector<size_t> dupInds;
        for (auto it = exported.begin(); it != exported.end(); ++it) {
            LexNext upperBound(it->parts());
            auto longer = it + 1;
            for (; longer != exported.end() && !(upperBound < *longer); ++longer) {
                if (auto e = ctx.beginError(longer->fqn.loc.offsets(), core::errors::Packager::ExportConflict)) {
                    if (it->parts() == longer->parts()) {
                        e.setHeader("Duplicate export of `{}`",
                                    fmt::map_join(longer->parts(), "::", [&](const auto &nr) { return nr.show(ctx); }));
                    } else {
                        e.setHeader("Cannot export `{}` because another exported name `{}` is a prefix of it",
                                    fmt::map_join(longer->parts(), "::", [&](const auto &nr) { return nr.show(ctx); }),
                                    fmt::map_join(it->parts(), "::", [&](const auto &nr) { return nr.show(ctx); }));
                    }
                    e.addErrorLine(it->fqn.loc, "Prefix exported here");
                }

                dupInds.emplace_back(distance(exported.begin(), longer));
            }
        }

        // Remove duplicates we found (in reverse order)
        fast_sort(dupInds);
        dupInds.erase(unique(dupInds.begin(), dupInds.end()), dupInds.end());
        for (auto indIt = dupInds.rbegin(); indIt != dupInds.rend(); ++indIt) {
            // Yes this is quadratic, but this only happens in an error condition.
            exported.erase(exported.begin() + *indIt);
        }

        ENFORCE(info.exports_.empty());
        std::swap(exported, info.exports_);
    }

    bool isSpecMethod(const sorbet::ast::Send &send) const {
        switch (send.fun.rawId()) {
            case core::Names::import().rawId():
            case core::Names::testImport().rawId():
            case core::Names::export_().rawId():
            case core::Names::restrictToService().rawId():
            case core::Names::visibleTo().rawId():
            case core::Names::exportAll().rawId():
                return true;
            default:
                return false;
        }
    }

    core::packages::ImportType method2ImportType(const ast::Send &send) const {
        switch (send.fun.rawId()) {
            case core::Names::import().rawId():
                return core::packages::ImportType::Normal;
            case core::Names::testImport().rawId():
                return core::packages::ImportType::Test;
            default:
                ENFORCE(false);
                Exception::notImplemented();
        }
    }

    /* Forbid arbitrary computation in packages */
    void illegalNode(core::Context ctx, const ast::ExpressionPtr &original) {
        if (auto e = ctx.beginError(original.loc(), core::errors::Packager::InvalidPackageExpression)) {
            e.setHeader("Invalid expression in package: `{}` not allowed", original.nodeName());
            e.addErrorNote("To learn about what's allowed in `{}` files, see http://go/package-layout", "__package.rb");
        }
    }

    void preTransformExpressionPtr(core::Context ctx, const ast::ExpressionPtr &original) {
        auto tag = original.tag();
        if ( // PackageSpec definition; handled above explicitly
            tag == ast::Tag::ClassDef ||
            // Various DSL methods; handled above explicitly
            tag == ast::Tag::Send ||
            // Arguments to DSL methods; always allowed
            tag == ast::Tag::UnresolvedConstantLit || tag == ast::Tag::ConstantLit || tag == ast::Tag::Literal ||
            // Technically only in scopes of constant literals, but easier to just always allow
            tag == ast::Tag::EmptyTree ||
            // Technically only as receiver of DSL method, but easier to just always allow
            original.isSelfReference()) {
            return;
        }

        illegalNode(ctx, original);
    }

private:
    optional<core::packages::StrictDependenciesLevel> parseStrictDependenciesOption(ast::ExpressionPtr &arg) {
        auto lit = ast::cast_tree<ast::Literal>(arg);
        if (!lit || !lit->isString()) {
            return nullopt;
        }
        auto value = lit->asString();

        if (value == core::Names::false_()) {
            return core::packages::StrictDependenciesLevel::False;
        } else if (value == core::Names::layered()) {
            return core::packages::StrictDependenciesLevel::Layered;
        } else if (value == core::Names::layeredDag()) {
            return core::packages::StrictDependenciesLevel::LayeredDag;
        } else if (value == core::Names::dag()) {
            return core::packages::StrictDependenciesLevel::Dag;
        }

        return nullopt;
    }

    optional<core::NameRef> parseLayerOption(const core::GlobalState &gs, ast::ExpressionPtr &arg) {
        auto validLayers = gs.packageDB().layers();
        auto lit = ast::cast_tree<ast::Literal>(arg);
        if (!lit || !lit->isString()) {
            return nullopt;
        }
        auto value = lit->asString();
        if (absl::c_find(validLayers, value) != validLayers.end()) {
            return value;
        }
        return nullopt;
    }
};

unique_ptr<PackageInfoImpl> definePackage(const core::GlobalState &gs, ast::ParsedFile &package) {
    ENFORCE(package.file.exists());
    ENFORCE(package.file.data(gs).isPackage(gs));
    // Assumption: Root of AST is <root> class. (This won't be true
    // for `typed: ignore` files, so we should make sure to catch that
    // elsewhere.)
    ENFORCE(ast::isa_tree<ast::ClassDef>(package.tree));
    ENFORCE(ast::cast_tree_nonnull<ast::ClassDef>(package.tree).symbol == core::Symbols::root());

    core::Context ctx(gs, core::Symbols::root(), package.file);

    auto &rootClass = ast::cast_tree_nonnull<ast::ClassDef>(package.tree);

    unique_ptr<PackageInfoImpl> info;
    bool reportedError = false;
    for (auto &rootStmt : rootClass.rhs) {
        if (info != nullptr) {
            // No error here; let the error be reported in the tree walk later as a bad node type.
            continue;
        }

        auto packageSpecClass = ast::cast_tree<ast::ClassDef>(rootStmt);
        if (packageSpecClass == nullptr) {
            // No error here; let this be reported in the tree walk later as a bad node type,
            // or at the end of this function if no PackageSpec is found.
            continue;
        }

        if (packageSpecClass->ancestors.size() != 1 ||
            !ast::isa_tree<ast::UnresolvedConstantLit>(packageSpecClass->name)) {
            mustContainPackageDef(ctx, packageSpecClass->declLoc);
            reportedError = true;
            continue;
        }

        auto &superClass = packageSpecClass->ancestors[0];
        auto superClassLit = ast::cast_tree<ast::UnresolvedConstantLit>(superClass);
        if (superClassLit == nullptr || superClassLit->cnst != core::Names::Constants::PackageSpec()) {
            mustContainPackageDef(ctx, superClass.loc());
            reportedError = true;
            continue;
        }

        // ---- Mutates the tree ----
        // We can't do these rewrites in rewriter, because this rewrite should only happen if
        // `opts.stripePackages` is set. That would mean we would have to add another cache flavor,
        // which would double the size of Sorbet's disk cache.
        //
        // Other than being able to say "we don't mutate the trees in packager" there's not much
        // value in going that far (even namer mutates the trees; the packager fills a similar role).

        // Pre-resolve the super class. This makes it easier to detect that this is a package
        // spec-related class def in later passes without having to recursively walk up the constant
        // lit's scope to find if it starts with <PackageSpecRegistry>.
        superClass = ast::make_expression<ast::ConstantLit>(core::Symbols::PackageSpec(),
                                                            superClass.toUnique<ast::UnresolvedConstantLit>());

        auto nameTree = ast::cast_tree<ast::UnresolvedConstantLit>(packageSpecClass->name);
        if (!validatePackageName(ctx, nameTree)) {
            reportedError = true;
            continue;
        }

        // `class Foo < PackageSpec` -> `class <PackageSpecRegistry>::Foo < PackageSpec`
        // This removes the PackageSpec's themselves from the top-level namespace
        packageSpecClass->name = prependName(move(packageSpecClass->name));

        info = make_unique<PackageInfoImpl>(getPackageName(ctx, nameTree), ctx.locAt(packageSpecClass->loc),
                                            ctx.locAt(packageSpecClass->declLoc));
    }

    // Only report an error if we didn't already
    // (the one we reported will have been more descriptive than this one)
    if (info == nullptr && !reportedError) {
        auto errLoc = rootClass.rhs.empty() ? core::LocOffsets{0, 0} : rootClass.rhs[0].loc();
        mustContainPackageDef(ctx, errLoc);
    }

    return info;
}

void rewritePackageSpec(const core::GlobalState &gs, ast::ParsedFile &package, PackageInfoImpl &info) {
    PackageSpecBodyWalk bodyWalk(info);
    core::Context ctx(gs, core::Symbols::root(), package.file);
    ast::TreeWalk::apply(ctx, bodyWalk, package.tree);
    if (gs.packageDB().enforceLayering()) {
        if (!bodyWalk.foundLayerDeclaration) {
            if (auto e = ctx.beginError(info.name.loc, core::errors::Packager::InvalidLayer)) {
                e.setHeader("This package does not declare a `{}`", "layer");
            }
        }
        if (!bodyWalk.foundStrictDependenciesDeclaration) {
            if (auto e = ctx.beginError(info.name.loc, core::errors::Packager::InvalidStrictDependencies)) {
                e.setHeader("This package does not declare a `{}` level", "strict_dependencies");
            }
        }
    }
    bodyWalk.finalize(ctx);
}

unique_ptr<PackageInfoImpl> createAndPopulatePackageInfo(core::GlobalState &gs, ast::ParsedFile &package) {
    auto info = definePackage(gs, package);
    if (info == nullptr) {
        return info;
    }
    populateMangledName(gs, info->name);

    rewritePackageSpec(gs, package, *info);
    for (auto &importedPackageName : info->importedPackageNames) {
        populateMangledName(gs, importedPackageName.name);

        if (importedPackageName.name.mangledName == info->name.mangledName) {
            if (auto e = gs.beginError(core::Loc(package.file, importedPackageName.name.loc),
                                       core::errors::Packager::NoSelfImport)) {
                string import_;
                switch (importedPackageName.type) {
                    case core::packages::ImportType::Normal:
                        import_ = "import";
                        break;
                    case core::packages::ImportType::Test:
                        import_ = "test_import";
                        break;
                }
                e.setHeader("Package `{}` cannot {} itself", info->name.toString(gs), import_);
            }
        }
    }

    for (auto &visibleTo : info->visibleTo_) {
        populateMangledName(gs, visibleTo.first);
    }

    auto extraPackageFilesDirectoryUnderscorePrefixes = gs.packageDB().extraPackageFilesDirectoryUnderscorePrefixes();
    auto extraPackageFilesDirectorySlashDeprecatedPrefixes =
        gs.packageDB().extraPackageFilesDirectorySlashDeprecatedPrefixes();
    auto extraPackageFilesDirectorySlashPrefixes = gs.packageDB().extraPackageFilesDirectorySlashPrefixes();

    const auto numPrefixes = extraPackageFilesDirectoryUnderscorePrefixes.size() +
                             extraPackageFilesDirectorySlashDeprecatedPrefixes.size() +
                             extraPackageFilesDirectorySlashPrefixes.size() + 1;
    info->packagePathPrefixes.reserve(numPrefixes);
    auto packageFilePath = package.file.data(gs).path();
    ENFORCE(FileOps::getFileName(packageFilePath) == PACKAGE_FILE_NAME);
    info->packagePathPrefixes.emplace_back(packageFilePath.substr(0, packageFilePath.find_last_of('/') + 1));
    const string_view shortName = info->name.mangledName.mangledName.shortName(gs);
    const string slashDirName = absl::StrJoin(info->name.fullName.parts, "/", core::packages::NameFormatter(gs)) + "/";
    const string_view dirNameFromShortName = shortName.substr(0, shortName.rfind(core::PACKAGE_SUFFIX));

    for (const string &prefix : extraPackageFilesDirectoryUnderscorePrefixes) {
        // Project_FooBar -- munge with underscore
        info->packagePathPrefixes.emplace_back(absl::StrCat(prefix, dirNameFromShortName, "/"));
    }

    for (const string &prefix : extraPackageFilesDirectorySlashDeprecatedPrefixes) {
        // project/Foo_bar -- convert camel-case to snake-case and munge with slash
        std::string additionalDirPath;
        additionalDirPath.reserve(prefix.size() + 2 * dirNameFromShortName.length() + 1);
        additionalDirPath += prefix;
        for (int i = 0; i < dirNameFromShortName.length(); i++) {
            if (dirNameFromShortName[i] == '_') {
                additionalDirPath.push_back('/');
            } else if (i == 0 || dirNameFromShortName[i - 1] == '_') {
                // Capitalizing first letter in each directory name to avoid conflicts with ignored directories,
                // which tend to be all lower case
                additionalDirPath.push_back(std::toupper(dirNameFromShortName[i]));
            } else {
                if (isupper(dirNameFromShortName[i])) {
                    additionalDirPath.push_back('_'); // snake-case munging
                }

                additionalDirPath.push_back(std::tolower(dirNameFromShortName[i]));
            }
        }
        additionalDirPath.push_back('/');

        info->packagePathPrefixes.emplace_back(std::move(additionalDirPath));
    }

    for (const string &prefix : extraPackageFilesDirectorySlashPrefixes) {
        // Project/FooBar -- each constant name is a file or directory name
        info->packagePathPrefixes.emplace_back(absl::StrCat(prefix, slashDirName));
    }

    return info;
}

// Metadata for Tarjan's algorithm
// https://www.cs.cmu.edu/~15451-f18/lectures/lec19-DFS-strong-components.pdf provides a good overview of the
// algorithm.
const int UNVISITED = 0;
struct NodeInfo {
    // A given package's index in the DFS traversal; ie. when it was first visited. The default value of 0 means the
    // package hasn't been visited yet.
    int index = UNVISITED;
    // The lowest index reachable from a given package (in the same SCC) by following any number of tree edges
    // and at most one back/cross edge
    int lowLink = 0;
    // Fast way to check if a package is on the stack
    bool onStack = false;
};

struct ComputeSCCsMetadata {
    int nextIndex = 1;
    int nextSCCId = 0;
    UnorderedMap<core::packages::MangledName, NodeInfo> nodeMap;
    // As we visit packages, we push them onto the stack. Once we find the "root" of an SCC, we can use the stack to
    // determine all packages in the SCC.
    std::vector<core::packages::MangledName> stack;
};

// DFS traversal for Tarjan's algorithm starting from pkgName, along with keeping track of some metadata needed for
// detecting SCCs.
void strongConnect(core::GlobalState &gs, ComputeSCCsMetadata &metadata, core::packages::MangledName pkgName,
                   NodeInfo &infoAtEntry) {
    auto *pkgInfoPtr = gs.packageDB().getPackageInfoNonConst(pkgName);
    if (!pkgInfoPtr) {
        // This is to handle the case where the user imports a package that doesn't exist.
        return;
    }
    auto &pkgInfo = PackageInfoImpl::from(*pkgInfoPtr);

    infoAtEntry.index = metadata.nextIndex;
    infoAtEntry.lowLink = metadata.nextIndex;
    metadata.nextIndex++;
    metadata.stack.push_back(pkgName);
    infoAtEntry.onStack = true;

    for (auto &i : pkgInfo.importedPackageNames) {
        if (i.type == core::packages::ImportType::Test) {
            continue;
        }
        // We need to be careful with this; it's not valid after a call to `strongConnect`,
        // because our reference might disappear from underneath us during that call.
        auto &importInfo = metadata.nodeMap[i.name.mangledName];
        if (importInfo.index == UNVISITED) {
            // This is a tree edge (ie. a forward edge that we haven't visited yet).
            strongConnect(gs, metadata, i.name.mangledName, importInfo);

            // Need to re-lookup for the reason above.
            auto &importInfo = metadata.nodeMap[i.name.mangledName];
            if (importInfo.index == UNVISITED) {
                // This is to handle early return above.
                continue;
            }
            // Since we can follow any number of tree edges for lowLink, the lowLink of child is valid for this package
            // too.
            //
            // Note that we cannot use `infoAtEntry` here because it might have been invalidated.
            auto &pkgLink = metadata.nodeMap[pkgName].lowLink;
            pkgLink = std::min(pkgLink, importInfo.lowLink);
        } else if (importInfo.onStack) {
            // This is a back edge (edge to ancestor) or cross edge (edge to a different subtree). Since we can only
            // follow at most one back/cross edge, the best update we can make to lowlink of the current package is the
            // child's index.
            //
            // Note that we cannot use `infoAtEntry` here because it might have been invalidated.
            auto &pkgLink = metadata.nodeMap[pkgName].lowLink;
            pkgLink = std::min(pkgLink, importInfo.index);
        }
        // If the child package is already visited and not on the stack, it's in a different SCC, so no update to the
        // lowlink.
    }

    // We cannot re-use `infoAtEntry` here because `nodeMap` might have been re-allocated and
    // invalidate our reference.
    auto &ourInfo = metadata.nodeMap[pkgName];
    if (ourInfo.index == ourInfo.lowLink) {
        // This is the root of an SCC. This means that all packages on the stack from this package to the top of the top
        // of the stack are in the same SCC. Pop the stack until we reach the root of the SCC, and assign them the same
        // SCC ID.
        core::packages::MangledName poppedPkgName;
        do {
            poppedPkgName = metadata.stack.back();
            metadata.stack.pop_back();
            metadata.nodeMap[poppedPkgName].onStack = false;
            PackageInfoImpl &poppedPkgInfo =
                PackageInfoImpl::from(*(gs.packageDB().getPackageInfoNonConst(poppedPkgName)));
            poppedPkgInfo.sccID = metadata.nextSCCId;
        } while (poppedPkgName != pkgName);
        metadata.nextSCCId++;
    }
}

// Tarjan's algorithm for finding strongly connected components
// NOTE: This function must be called every time a non-test import is added or removed from a package.
// It is relatively fast, so calling it on every __package.rb edit is an okay overapproximation for simplicity.
void computeSCCs(core::GlobalState &gs) {
    Timer timeit(gs.tracer(), "packager::computeSCCs");
    ComputeSCCsMetadata metadata;
    auto allPackages = gs.packageDB().packages();
    metadata.stack.reserve(allPackages.size());
    metadata.nodeMap.reserve(allPackages.size());
    for (auto package : allPackages) {
        auto &info = metadata.nodeMap[package];
        if (info.index == UNVISITED) {
            strongConnect(gs, metadata, package, info);
        }
    }
}

void validateLayering(const core::Context &ctx, const Import &i) {
    if (i.type == core::packages::ImportType::Test) {
        return;
    }

    const auto &packageDB = ctx.state.packageDB();
    ENFORCE(packageDB.getPackageInfo(i.name.mangledName).exists())
    ENFORCE(packageDB.getPackageForFile(ctx, ctx.file).exists())
    auto &thisPkg = PackageInfoImpl::from(packageDB.getPackageForFile(ctx, ctx.file));
    auto &otherPkg = PackageInfoImpl::from(packageDB.getPackageInfo(i.name.mangledName));
    ENFORCE(thisPkg.sccID.has_value(), "computeSCCs should already have been called and set sccID");
    ENFORCE(otherPkg.sccID.has_value(), "computeSCCs should already have been called and set sccID");

    if (!thisPkg.strictDependenciesLevel().has_value() || !otherPkg.strictDependenciesLevel().has_value() ||
        !thisPkg.layer().has_value() || !otherPkg.layer().has_value()) {
        return;
    }

    if (thisPkg.strictDependenciesLevel().value().first == core::packages::StrictDependenciesLevel::False) {
        return;
    }

    auto pkgLayer = thisPkg.layer().value().first;
    auto otherPkgLayer = otherPkg.layer().value().first;

    if (thisPkg.causesLayeringViolation(packageDB, otherPkgLayer)) {
        if (auto e = ctx.beginError(i.name.loc, core::errors::Packager::LayeringViolation)) {
            e.setHeader("Layering violation: cannot import `{}` (in layer `{}`) from `{}` (in layer `{}`)",
                        otherPkg.show(ctx), otherPkgLayer.show(ctx), thisPkg.show(ctx), pkgLayer.show(ctx));
            e.addErrorLine(core::Loc(thisPkg.loc.file(), thisPkg.layer().value().second), "`{}`'s `{}` declared here",
                           thisPkg.show(ctx), "layer");
            e.addErrorLine(core::Loc(otherPkg.loc.file(), otherPkg.layer().value().second), "`{}`'s `{}` declared here",
                           otherPkg.show(ctx), "layer");
            // TODO: if the import is unused (ie. there are no references in this package to the imported package),
            // autocorrect to delete import
        }
    }

    core::packages::StrictDependenciesLevel otherPkgExpectedLevel = thisPkg.minimumStrictDependenciesLevel();

    if (otherPkg.strictDependenciesLevel().value().first < otherPkgExpectedLevel) {
        if (auto e = ctx.beginError(i.name.loc, core::errors::Packager::StrictDependenciesViolation)) {
            e.setHeader("Strict dependencies violation: All of `{}`'s `{}`s must be `{}` or higher", thisPkg.show(ctx),
                        "import", core::packages::strictDependenciesLevelToString(otherPkgExpectedLevel));
            e.addErrorLine(core::Loc(otherPkg.loc.file(), otherPkg.strictDependenciesLevel().value().second),
                           "`{}`'s `{}` level declared here", otherPkg.show(ctx), "strict_dependencies");
            // TODO: if the import is unused (ie. there are no references in this package to the imported package),
            // autocorrect to delete import
            // TODO: if the imported package can be trivially upgraded to the required level (ex. it's at 'false' but
            // has no layering violations), autocorrect to do so
        }
    }

    if (thisPkg.strictDependenciesLevel().value().first >= core::packages::StrictDependenciesLevel::LayeredDag) {
        if (thisPkg.sccID == otherPkg.sccID) {
            if (auto e = ctx.beginError(i.name.loc, core::errors::Packager::StrictDependenciesViolation)) {
                auto level = fmt::format(
                    "strict_dependencies '{}'",
                    core::packages::strictDependenciesLevelToString(thisPkg.strictDependenciesLevel().value().first));
                e.setHeader("Strict dependencies violation: importing `{}` will put `{}` into a cycle, which is not "
                            "valid at `{}`",
                            otherPkg.show(ctx), thisPkg.show(ctx), level);
            }
            // TODO: if the import is unused (ie. there are no references in this package to the imported package),
            // autocorrect to delete import
        }
    }
}

void validateVisibility(const core::Context &ctx, const Import i) {
    ENFORCE(ctx.state.packageDB().getPackageInfo(i.name.mangledName).exists())
    ENFORCE(ctx.state.packageDB().getPackageForFile(ctx, ctx.file).exists())
    auto &absPkg = ctx.state.packageDB().getPackageForFile(ctx, ctx.file);
    auto &otherPkg = ctx.state.packageDB().getPackageInfo(i.name.mangledName);

    const auto &visibleTo = otherPkg.visibleTo();
    if (visibleTo.empty() && !otherPkg.visibleToTests()) {
        return;
    }

    if (otherPkg.visibleToTests() && i.type == core::packages::ImportType::Test) {
        return;
    }

    bool allowed =
        absl::c_any_of(visibleTo, [&absPkg](const auto &other) { return visibilityApplies(other, absPkg.fullName()); });

    if (!allowed) {
        if (auto e = ctx.beginError(i.name.loc, core::errors::Packager::ImportNotVisible)) {
            e.setHeader("Package `{}` includes explicit visibility modifiers and cannot be imported from `{}`",
                        otherPkg.show(ctx), absPkg.show(ctx));
            e.addErrorNote("Please consult with the owning team before adding a `{}` line to the package `{}`",
                           "visible_to", otherPkg.show(ctx));
        }
    }
}

} // namespace

void validatePackage(core::Context ctx) {
    const auto &packageDB = ctx.state.packageDB();
    auto &absPkg = packageDB.getPackageForFile(ctx, ctx.file);
    if (!absPkg.exists()) {
        // We already produced an error on this package when producing its package info.
        // The correct course of action is to abort the transform.
        return;
    }

    // Sanity check: __package.rb files _must_ be typed: strict
    if (ctx.file.data(ctx).originalSigil < core::StrictLevel::Strict) {
        if (auto e = ctx.beginError(core::LocOffsets{0, 0}, core::errors::Packager::PackageFileMustBeStrict)) {
            e.setHeader("Package files must be at least `{}`", "# typed: strict");
            // TODO(neil): Autocorrect to update the sigil?
        }
    }

    auto &pkgInfo = PackageInfoImpl::from(absPkg);
    bool skipImportVisibilityCheck = packageDB.allowRelaxedPackagerChecksFor(pkgInfo.mangledName());
    auto enforceLayering = ctx.state.packageDB().enforceLayering();

    if (skipImportVisibilityCheck && !enforceLayering) {
        return;
    }

    for (auto &i : pkgInfo.importedPackageNames) {
        auto &otherPkg = packageDB.getPackageInfo(i.name.mangledName);

        // this might mean the other package doesn't exist, but that should have been caught already
        if (!otherPkg.exists()) {
            continue;
        }

        if (enforceLayering) {
            validateLayering(ctx, i);
        }

        if (!skipImportVisibilityCheck) {
            validateVisibility(ctx, i);
        }
    }
}

void validatePackagedFile(core::Context ctx, const ast::ExpressionPtr &tree) {
    auto &file = ctx.file.data(ctx);
    ENFORCE(!file.isPackage(ctx));

    if (file.isPayload()) {
        // Files in Sorbet's payload are parsed and loaded in the --store-state phase, which runs
        // outside of the packager mode. They're allowed to not belong to a package.
        //
        // Note that other RBIs that are not in Sorbet's payload follow the normal packaging rules.
        //
        // We normally skip running the packager when building in sorbet-orig mode, which computes
        // the stored state, but payload files can be retypechecked by the fast path during LSP.
        return;
    }

    auto &pkg = ctx.state.packageDB().getPackageForFile(ctx, ctx.file);
    if (!pkg.exists()) {
        // Don't transform, but raise an error on the first line.
        if (auto e = ctx.beginError(core::LocOffsets{0, 0}, core::errors::Packager::UnpackagedFile)) {
            e.setHeader("File `{}` does not belong to a package; add a `{}` file to one "
                        "of its parent directories",
                        ctx.file.data(ctx).path(), PACKAGE_FILE_NAME);
        }
        return;
    }

    auto &pkgImpl = PackageInfoImpl::from(pkg);

    EnforcePackagePrefix enforcePrefix(ctx, pkgImpl);
    ast::ConstShallowWalk::apply(ctx, enforcePrefix, tree);
}

void Packager::findPackages(core::GlobalState &gs, absl::Span<ast::ParsedFile> files) {
    // Ensure files are in canonical order.
    // TODO(jez) Is this sort redundant? Should we move this sort to callers?
    fast_sort(files, [](const auto &a, const auto &b) -> bool { return a.file < b.file; });

    // Find packages and determine their imports/exports.
    {
        Timer timeit(gs.tracer(), "packager.findPackages");
        core::UnfreezeNameTable unfreeze(gs);
        core::packages::UnfreezePackages packages = gs.unfreezePackages();
        for (auto &file : files) {
            if (!file.file.data(gs).isPackage(gs)) {
                continue;
            }

            core::MutableContext ctx(gs, core::Symbols::root(), file.file);
            auto pkg = createAndPopulatePackageInfo(ctx, file);
            if (pkg == nullptr) {
                // There was an error creating a PackageInfoImpl for this file, and getPackageInfo has already
                // surfaced that error to the user. Nothing to do here.
                continue;
            }
            auto &prevPkg = gs.packageDB().getPackageInfo(pkg->mangledName());
            if (prevPkg.exists() && prevPkg.declLoc() != pkg->declLoc()) {
                if (auto e = ctx.beginError(pkg->loc.offsets(), core::errors::Packager::RedefinitionOfPackage)) {
                    auto pkgName = pkg->name.toString(ctx);
                    e.setHeader("Redefinition of package `{}`", pkgName);
                    e.addErrorLine(prevPkg.declLoc(), "Package `{}` originally defined here", pkgName);
                }
            } else {
                packages.db.enterPackage(move(pkg));
            }
        }
    }
}

void Packager::setPackageNameOnFiles(core::GlobalState &gs, absl::Span<const ast::ParsedFile> files) {
    std::vector<std::pair<core::FileRef, core::packages::MangledName>> mapping;
    mapping.reserve(files.size());

    {
        auto &db = gs.packageDB();
        for (auto &f : files) {
            auto &pkg = db.getPackageForFile(gs, f.file);
            if (!pkg.exists()) {
                continue;
            }

            mapping.emplace_back(f.file, pkg.mangledName());
        }
    }

    {
        auto packages = gs.unfreezePackages();
        for (auto [file, package] : mapping) {
            packages.db.setPackageNameForFile(file, package);
        }
    }
}

void Packager::setPackageNameOnFiles(core::GlobalState &gs, absl::Span<const core::FileRef> files) {
    std::vector<std::pair<core::FileRef, core::packages::MangledName>> mapping;
    mapping.reserve(files.size());

    {
        auto &db = gs.packageDB();
        for (auto &f : files) {
            auto &pkg = db.getPackageForFile(gs, f);
            if (!pkg.exists()) {
                continue;
            }

            mapping.emplace_back(f, pkg.mangledName());
        }
    }

    {
        auto packages = gs.unfreezePackages();
        for (auto [file, package] : mapping) {
            packages.db.setPackageNameForFile(file, package);
        }
    }
}

namespace {

enum class PackagerMode {
    PackagesOnly,
    PackagedFilesOnly,
    AllFiles,
};

template <PackagerMode Mode>
void packageRunCore(core::GlobalState &gs, WorkerPool &workers, absl::Span<ast::ParsedFile> files) {
    ENFORCE(!gs.cacheSensitiveOptions.runningUnderAutogen, "Packager pass does not run in autogen");

    Timer timeit(gs.tracer(), "packager");

    switch (Mode) {
        case PackagerMode::PackagesOnly:
            timeit.setTag("mode", "packages_only");
            break;
        case PackagerMode::PackagedFilesOnly:
            timeit.setTag("mode", "packaged_files_only");
            break;
        case PackagerMode::AllFiles:
            break;
    }

    constexpr bool buildPackageDB = Mode == PackagerMode::PackagesOnly || Mode == PackagerMode::AllFiles;
    constexpr bool validatePackagedFiles = Mode == PackagerMode::PackagedFilesOnly || Mode == PackagerMode::AllFiles;

    if constexpr (buildPackageDB) {
        Packager::findPackages(gs, files);
    }

    Packager::setPackageNameOnFiles(gs, files);

    {
        Timer timeit(gs.tracer(), "packager.rewritePackagesAndFiles");

        auto taskq = std::make_shared<ConcurrentBoundedQueue<size_t>>(files.size());
        absl::BlockingCounter barrier(max(workers.size(), 1));

        for (size_t i = 0; i < files.size(); ++i) {
            taskq->push(i, 1);
        }

        if constexpr (buildPackageDB) {
            if (gs.packageDB().enforceLayering()) {
                computeSCCs(gs);
            }
        }

        workers.multiplexJob("rewritePackagesAndFiles", [&gs, &files, &barrier, taskq]() {
            Timer timeit(gs.tracer(), "packager.rewritePackagesAndFilesWorker");
            size_t idx;
            for (auto result = taskq->try_pop(idx); !result.done(); result = taskq->try_pop(idx)) {
                if (!result.gotItem()) {
                    continue;
                }

                ast::ParsedFile &job = files[idx];
                auto file = job.file;
                core::Context ctx(gs, core::Symbols::root(), file);

                if (file.data(gs).isPackage(gs)) {
                    ENFORCE(buildPackageDB);
                    validatePackage(ctx);
                } else {
                    ENFORCE(validatePackagedFiles);
                    validatePackagedFile(ctx, job.tree);
                }
            }

            barrier.DecrementCount();
        });

        barrier.Wait();
    }
}
} // namespace

void Packager::run(core::GlobalState &gs, WorkerPool &workers, absl::Span<ast::ParsedFile> files) {
    packageRunCore<PackagerMode::AllFiles>(gs, workers, files);
}

vector<ast::ParsedFile> Packager::runIncremental(const core::GlobalState &gs, vector<ast::ParsedFile> files,
                                                 WorkerPool &workers) {
    // Note: This will only run if packages have not been changed (byte-for-byte equality).
    // TODO(nroman-stripe) This could be further incrementalized to avoid processing all packages by
    // building in an understanding of the dependencies between packages.
    Timer timeit(gs.tracer(), "packager.runIncremental");

    auto taskq = std::make_shared<ConcurrentBoundedQueue<size_t>>(files.size());
    absl::BlockingCounter barrier(max(workers.size(), 1));

    for (size_t i = 0; i < files.size(); ++i) {
        taskq->push(i, 1);
    }
    workers.multiplexJob("validatePackagesAndFiles", [&gs, &files, &barrier, taskq]() {
        size_t idx;
        for (auto result = taskq->try_pop(idx); !result.done(); result = taskq->try_pop(idx)) {
            if (!result.gotItem()) {
                continue;
            }

            ast::ParsedFile &file = files[idx];
            core::Context ctx(gs, core::Symbols::root(), file.file);
            if (file.file.data(gs).isPackage(gs)) {
                // Only rewrites the `__package.rb` file to mention `<PackageSpecRegistry>` and
                // report some syntactic packager errors.
                auto info = definePackage(gs, file);
                if (info != nullptr) {
                    rewritePackageSpec(gs, file, *info);
                }
                validatePackage(ctx);
            } else {
                validatePackagedFile(ctx, file.tree);
            }
        }
        barrier.DecrementCount();
    });
    barrier.Wait();
    return files;
}

void Packager::buildPackageDB(core::GlobalState &gs, WorkerPool &workers, absl::Span<ast::ParsedFile> files) {
    packageRunCore<PackagerMode::PackagesOnly>(gs, workers, files);
}

void Packager::validatePackagedFiles(core::GlobalState &gs, WorkerPool &workers, absl::Span<ast::ParsedFile> files) {
    packageRunCore<PackagerMode::PackagedFilesOnly>(gs, workers, files);
}

namespace {

struct PackageFiles {
    vector<core::FileRef> files;
    vector<core::FileRef> testFiles;
};

} // namespace

void Packager::dumpPackageInfo(const core::GlobalState &gs, std::string outputFile) {
    const auto &pkgDB = gs.packageDB();
    // package => files
    UnorderedMap<core::packages::MangledName, PackageFiles> packageFiles;
    for (uint32_t i = 1; i < gs.filesUsed(); ++i) {
        core::FileRef file(i);
        const auto &pkg = pkgDB.getPackageForFile(gs, file);
        if (pkg.exists()) {
            if (file.data(gs).isPackagedTest()) {
                packageFiles[pkg.mangledName()].testFiles.emplace_back(file);
            } else {
                packageFiles[pkg.mangledName()].files.emplace_back(file);
            }
        }
    }

    rapidjson::StringBuffer result;
    rapidjson::Writer writer(result);

    writer.StartArray();

    for (auto mangledName : pkgDB.packages()) {
        const auto &pkg = gs.packageDB().getPackageInfo(mangledName);

        writer.StartObject();

        writer.String("name");
        writer.String(absl::StrJoin(pkg.fullName(), "::", core::packages::NameFormatter(gs)));

        writer.String("imports");
        writer.StartArray();
        for (auto &import : pkg.imports()) {
            writer.String(absl::StrJoin(import, "::", core::packages::NameFormatter(gs)));
        }
        writer.EndArray();

        writer.String("testImports");
        writer.StartArray();
        for (auto &import : pkg.testImports()) {
            writer.String(absl::StrJoin(import, "::", core::packages::NameFormatter(gs)));
        }
        writer.EndArray();

        const auto it = packageFiles.find(mangledName);

        writer.String("files");
        writer.StartArray();
        if (it != packageFiles.end()) {
            for (auto file : it->second.files) {
                auto path = file.data(gs).path();
                writer.String(path.data(), path.size());
            }
        }
        writer.EndArray();

        writer.String("testFiles");
        writer.StartArray();
        if (it != packageFiles.end()) {
            for (auto file : it->second.testFiles) {
                auto path = file.data(gs).path();
                writer.String(path.data(), path.size());
            }
        }
        writer.EndArray();

        writer.EndObject();
    }

    writer.EndArray();

    FileOps::write(outputFile, result.GetString());
}

} // namespace sorbet::packager
