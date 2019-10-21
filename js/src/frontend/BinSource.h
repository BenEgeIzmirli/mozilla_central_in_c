/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BinSource_h
#define frontend_BinSource_h

/**
 * A Binary AST parser.
 *
 * At the time of this writing, this parser implements the grammar of ES5
 * and trusts its input (in particular, variable declarations).
 */

#include "mozilla/Maybe.h"

#include "frontend/BinTokenReaderTester.h"
#include "frontend/FullParseHandler.h"
#include "frontend/ParseContext.h"
#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"

#include "js/GCHashTable.h"
#include "js/GCVector.h"
#include "js/Result.h"

namespace js {
namespace frontend {

class BinASTParser;

/**
 * The parser for a Binary AST.
 *
 * By design, this parser never needs to backtrack or look ahead. Errors are not
 * recoverable.
 */
class BinASTParser : private JS::AutoGCRooter, public ErrorReporter
{
    using Tokenizer = BinTokenReaderTester;
    using BinFields = Tokenizer::BinFields;
    using Chars = Tokenizer::Chars;
    using Names = JS::GCVector<JSString*, 8>;

  public:
    BinASTParser(JSContext* cx, LifoAlloc& alloc, UsedNameTracker& usedNames, const JS::ReadOnlyCompileOptions& options)
        : AutoGCRooter(cx, BINPARSER)
        , traceListHead_(nullptr)
        , options_(options)
        , cx_(cx)
        , alloc_(alloc)
        , nodeAlloc_(cx, alloc)
        , keepAtoms_(cx)
        , parseContext_(nullptr)
        , usedNames_(usedNames)
        , factory_(cx, alloc, nullptr, SourceKind::Binary)
    {
         cx_->frontendCollectionPool().addActiveCompilation();
         tempPoolMark_ = alloc.mark();
    }
    ~BinASTParser()
    {
        alloc_.release(tempPoolMark_);

        /*
         * The parser can allocate enormous amounts of memory for large functions.
         * Eagerly free the memory now (which otherwise won't be freed until the
         * next GC) to avoid unnecessary OOMs.
         */
        alloc_.freeAllIfHugeAndUnused();

        cx_->frontendCollectionPool().removeActiveCompilation();
    }

    /**
     * Parse a buffer, returning a node (which may be nullptr) in case of success
     * or Nothing() in case of error.
     *
     * The instance of `ParseNode` MAY NOT survive the `BinASTParser`. Indeed,
     * destruction of the `BinASTParser` will also destroy the `ParseNode`.
     *
     * In case of error, the parser reports the JS error.
     */
    JS::Result<ParseNode*> parse(const uint8_t* start, const size_t length);
    JS::Result<ParseNode*> parse(const Vector<uint8_t>& data);

  private:
    MOZ_MUST_USE JS::Result<ParseNode*> parseAux(const uint8_t* start, const size_t length);

    // --- Raise errors.
    //
    // These methods return a (failed) JS::Result for convenience.

    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseMissingVariableInAssertedScope(JSAtom* name);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseMissingDirectEvalInAssertedScope();
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseInvalidKind(const char* superKind,
        const BinKind kind);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseInvalidField(const char* kind,
        const BinField field);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseInvalidNumberOfFields(
        const BinKind kind, const uint32_t expected, const uint32_t got);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseInvalidEnum(const char* kind,
        const Chars& value);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseMissingField(const char* kind,
        const BinField field);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseEmpty(const char* description);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseOOM();
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseError(const char* description);
    MOZ_MUST_USE mozilla::GenericErrorResult<JS::Error&> raiseError(BinKind kind,
        const char* description);


    // Ensure that this parser will never be used again.
    void poison();

    // Auto-generated methods
#include "frontend/BinSource-auto.h"

    // --- Auxiliary parsing functions
    template<size_t N>
    JS::Result<Ok, JS::Error&>
    checkFields(const BinKind kind, const BinFields& actual, const BinField (&expected)[N]);
    JS::Result<Ok, JS::Error&>
    checkFields0(const BinKind kind, const BinFields& actual);

    JS::Result<ParseNode*>
    buildFunction(const size_t start, const BinKind kind, ParseNode* name, ParseNode* params,
        ParseNode* body, FunctionBox* funbox);
    JS::Result<FunctionBox*>
    buildFunctionBox(GeneratorKind generatorKind, FunctionAsyncKind functionAsyncKind);

    // Parse full scope information to a specific var scope / let scope combination.
    MOZ_MUST_USE JS::Result<Ok> parseAndUpdateScope(ParseContext::Scope& varScope,
        ParseContext::Scope& letScope);
    // Parse a list of names and add it to a given scope.
    MOZ_MUST_USE JS::Result<Ok> parseAndUpdateScopeNames(ParseContext::Scope& scope,
        DeclarationKind kind);
    MOZ_MUST_USE JS::Result<Ok> parseAndUpdateCapturedNames();
    MOZ_MUST_USE JS::Result<Ok> checkBinding(JSAtom* name);

    // --- Utilities.

    MOZ_MUST_USE JS::Result<ParseNode*> appendDirectivesToBody(ParseNode* body,
        ParseNode* directives);

    // Read a string
    MOZ_MUST_USE JS::Result<Ok> readString(Chars& out);
    MOZ_MUST_USE JS::Result<Ok> readMaybeString(Maybe<Chars>& out);
    MOZ_MUST_USE JS::Result<Ok> readString(MutableHandleAtom out);
    MOZ_MUST_USE JS::Result<Ok> readMaybeString(MutableHandleAtom out);
    MOZ_MUST_USE JS::Result<bool> readBool();
    MOZ_MUST_USE JS::Result<double> readNumber();

    const ReadOnlyCompileOptions& options() const override {
        return this->options_;
    }

    // Names


    bool hasUsedName(HandlePropertyName name);

    // --- GC.

    void trace(JSTracer* trc) {
        ObjectBox::TraceList(trc, traceListHead_);
    }


  public:
    ObjectBox* newObjectBox(JSObject* obj) {
        MOZ_ASSERT(obj);

        /*
         * We use JSContext.tempLifoAlloc to allocate parsed objects and place them
         * on a list in this Parser to ensure GC safety. Thus the tempLifoAlloc
         * arenas containing the entries must be alive until we are done with
         * scanning, parsing and code generation for the whole script or top-level
         * function.
         */

         ObjectBox* objbox = alloc_.new_<ObjectBox>(obj, traceListHead_);
         if (!objbox) {
             ReportOutOfMemory(cx_);
             return nullptr;
        }

        traceListHead_ = objbox;

        return objbox;
    }

    ParseNode* allocParseNode(size_t size) {
        MOZ_ASSERT(size == sizeof(ParseNode));
        return static_cast<ParseNode*>(nodeAlloc_.allocNode());
    }

    JS_DECLARE_NEW_METHODS(new_, allocParseNode, inline)

  private: // Implement ErrorReporter

    virtual void lineAndColumnAt(size_t offset, uint32_t* line, uint32_t* column) const override {
        *line = 0;
        *column = offset;
    }
    virtual void currentLineAndColumn(uint32_t* line, uint32_t* column) const override {
        *line = 0;
        *column = offset();
    }
    size_t offset() const {
        if (tokenizer_.isSome())
            return tokenizer_->offset();

        return 0;
    }
    virtual bool hasTokenizationStarted() const override {
        return tokenizer_.isSome();
    }
    virtual void reportErrorNoOffsetVA(unsigned errorNumber, va_list args) override;
    virtual const char* getFilename() const override {
        return this->options_.filename();
    }

    ObjectBox* traceListHead_;
    const ReadOnlyCompileOptions& options_;
    JSContext* cx_;
    LifoAlloc& alloc_;
    LifoAlloc::Mark tempPoolMark_;
    ParseNodeAllocator nodeAlloc_;

    // Root atoms and objects allocated for the parse tree.
    AutoKeepAtoms keepAtoms_;

    // The current ParseContext, holding directives, etc.
    ParseContext* parseContext_;
    UsedNameTracker& usedNames_;
    Maybe<Tokenizer> tokenizer_;
    FullParseHandler factory_;
    VariableDeclarationKind variableDeclarationKind_;

    friend class BinParseContext;
    friend class AutoVariableDeclarationKind;

    // Needs access to AutoGCRooter.
    friend void TraceBinParser(JSTracer* trc, AutoGCRooter* parser);
};

class BinParseContext : public ParseContext
{
  public:
    BinParseContext(JSContext* cx, BinASTParser* parser, SharedContext* sc,
        Directives* newDirectives)
        : ParseContext(cx, parser->parseContext_, sc, *parser,
                       parser->usedNames_, newDirectives, /* isFull = */ true)
    { }
};


} // namespace frontend
} // namespace js

#endif // frontend_BinSource_h