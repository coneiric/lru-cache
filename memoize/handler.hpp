/**
* The MIT License (MIT)
* Copyright (c) 2016 Peter Goldsborough and Markus Engel
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <cstddef>
#include <string>

#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"

namespace Memoize {

/// Handles a match for a `memoize` annotation in the AST.
///
/// Given a matched function definition, this class will rewrite the original
/// source code in a way that wraps the matched function inside a new function,
/// which also stores a cache to replace some function calls with faster symbol
/// table lookups. More precisely, given a function *definition* like the
/// following:
///
/// \code{.cpp}
/// int f(int x, float y, char z) { return x + y + z; }
/// \endcode
///
/// The handler will replace this with new source code of the following form:
///
/// 1. The original function is declared before its definition.
/// 2. The original function is renamed to a mangled name.
/// 3. A new definition for the original function, under its original, unmangled
/// name, is appended. This new definition stores a cache and has access to the
/// original function to compute its return value in the case of a cache miss.
///
/// All other code referencing the original code, including any other function
/// calls as well as declaration, are unaffected by this chang.
class MemoizeHandler : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  using MatchResult = clang::ast_matchers::MatchFinder::MatchResult;

  explicit MemoizeHandler(clang::Rewriter& Rewriter) : Rewriter(Rewriter) {}

  /// Performs the appropriate rewriting on a matched function.
  ///
  /// \param Result A 'MatchResult' representing the matched function.
  void run(const MatchResult& Result) override {
    // Set internal state
    SourceManager = Result.SourceManager;
    LanguageOptions = &(Result.Context->getLangOpts());

    // The matched function.
    const auto& Function =
        *(Result.Nodes.getNodeAs<clang::FunctionDecl>("target"));

    assert(Function != nullptr);

    const auto NewName = renameOriginalFunction(Function);
    const auto Prototype = getFunctionPrototype(Function);

    auto NewDefinition = createMemoizedDefinition(Function, Prototype, NewName);
    auto AfterOriginalFunction = Function.getLocEnd().getLocWithOffset(1);
    Rewriter.InsertTextAfter(AfterOriginalFunction, NewDefinition);

    // Redeclare the function before its original definition
    // so that recursive calls can see the declaration.
    // Note that this has to be done *after* the other changes, as inserting
    // it would otherwise offset all further source locations.
    Rewriter.InsertTextBefore(Function.getLocStart(), Prototype + ";\n");
  }

private:
  using size_t = std::size_t;

  /// Creates the memoized definition for the original function declaration.
  ///
  /// \param Function The matched function.
  /// \param Prototype The prototype string of the original function.
  /// \param NewName The new name of the original function.
  ///
  /// \return A string holding the entire new function definition.
  std::string createMemoizedDefinition(const clang::FunctionDecl& Function,
                                       const std::string& Prototype,
                                       const std::string& NewName) const {
    // Add our new, memoized definition for the function under its original name
    std::string MemoizedDefinition;
    MemoizedDefinition.reserve(75);

    MemoizedDefinition += "\n\n";
    MemoizedDefinition += Prototype;
    MemoizedDefinition += " {\nstatic const auto proxy = memoize(";
    MemoizedDefinition += NewName;
    MemoizedDefinition += ");\nreturn proxy(";
    MemoizedDefinition += getParameterNames(Function);
    MemoizedDefinition += ");\n}";

    return MemoizedDefinition;
  }

  /// Collects the names of the parameters of a function.
  ///
  /// Given a function like
  /// \code{.cpp}
  /// int f(int x, float y, char z) { ... }
  /// \endcode
  /// This function will return a comma-separated list of the function
  /// parmeters' names, e.g. 'x, y, z' for this example. This string may be used
  /// for a function call.
  ///
  /// \param Function The function whose parameter names to extract.
  /// \return A comma-separated list of the function's parameter names.
  std::string getParameterNames(const clang::FunctionDecl& Function) const {
    std::string ParameterNames;

    // Reserve an upper bound of space for the names in a
    // parameter list (usually shorter without types)
    ParameterNames.reserve(128);

    size_t index = 0;
    for (const auto* Parameter : Function.parameters()) {
      ParameterNames += Parameter->getNameAsString();
      if (++index < Function.getNumParams()) {
        ParameterNames += ", ";
      }
    }

    return ParameterNames;
  }

  /// Renames the given function in the AST and returns its new name.
  ///
  /// \param Function The function to rename.
  ///
  /// \return The new name of the function (the original name + '__original__').
  std::string
  renameOriginalFunction(const clang::FunctionDecl& Function) const {
    const auto OriginalName = Function.getNameAsString();
    const auto NewName = OriginalName + "__original__";

    const auto BeforeParameters =
        Function.getLocation().getLocWithOffset(OriginalName.length() - 1);

    const auto DeclarationBegin = Function.getLocStart();

    const auto ReturnType = Function.getReturnType().getAsString();
    const auto NewDeclaration = ReturnType + " " + NewName;

    Rewriter.ReplaceText({DeclarationBegin, BeforeParameters}, NewDeclaration);

    return NewName;
  }

  /// Collects the prototype of a function, as a string.
  ///
  /// Given a function like
  /// \code{.cpp}
  /// int f(int x, float y, char z) { ... }
  /// \endcode
  /// this function will return its prototype, e.g. `int f(int x, float y,
  /// char z)` for this example. *No* terminator (like a semicolon) is appended.
  ///
  /// \return The prototype of the function, as a string.
  std::string getFunctionPrototype(const clang::FunctionDecl& Function) const {
    std::string Prototype;
    Prototype.reserve(256);

    const auto Name = Function.getNameAsString();

    Prototype += Function.getReturnType().getAsString() + " ";
    Prototype += Name;
    Prototype += getParameterList(Function, Name);

    return Prototype;
  }

  /// Returns the parameter list of a function, as a string.
  ///
  /// Given a function like
  /// \code{.cpp}
  /// int f(int x, float y, char z) { ... }
  /// \endcode
  /// this function will return a comma-separated list of the function's
  /// parameters, including their type and enclosed in parentheses. For this
  /// example, the returned string would be `"(int x, float y, char z)"`.
  ///
  /// \return The parameter list of the function, as a string.
  std::string getParameterList(const clang::FunctionDecl& Function,
                               const std::string& Name) const {
    const auto BeforeParametersLocation =
        Function.getLocation().getLocWithOffset(Name.length());
    const auto* BeforeParameters =
        SourceManager->getCharacterData(BeforeParametersLocation);

    const auto AfterParametersLocation =
        Function.getBody()->getLocStart().getLocWithOffset(-1);
    const auto* AfterParameters =
        SourceManager->getCharacterData(AfterParametersLocation);

    return std::string(BeforeParameters, AfterParameters);
  }

  /// The `Rewriter` instance used to modify the source code.
  clang::Rewriter& Rewriter;

  /// The `SourceManager` to access the AST's source code.
  const clang::SourceManager* SourceManager;

  /// The `LanguageOptions` for an AST.
  const clang::LangOptions* LanguageOptions;
};

}  // namespace Memoize