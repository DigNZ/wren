#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "common.h"
#include "compiler.h"

#define MAX_NAME 256

typedef enum
{
  TOKEN_LEFT_PAREN,
  TOKEN_RIGHT_PAREN,
  TOKEN_LEFT_BRACKET,
  TOKEN_RIGHT_BRACKET,
  TOKEN_LEFT_BRACE,
  TOKEN_RIGHT_BRACE,
  TOKEN_COLON,
  TOKEN_DOT,
  TOKEN_COMMA,
  TOKEN_STAR,
  TOKEN_SLASH,
  TOKEN_PERCENT,
  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_PIPE,
  TOKEN_AMP,
  TOKEN_BANG,
  TOKEN_EQ,
  TOKEN_LT,
  TOKEN_GT,
  TOKEN_LTEQ,
  TOKEN_GTEQ,
  TOKEN_EQEQ,
  TOKEN_BANGEQ,

  TOKEN_CLASS,
  TOKEN_ELSE,
  TOKEN_FALSE,
  TOKEN_FN,
  TOKEN_IF,
  TOKEN_IS,
  TOKEN_NULL,
  TOKEN_STATIC,
  TOKEN_THIS,
  TOKEN_TRUE,
  TOKEN_VAR,

  TOKEN_NAME,
  TOKEN_NUMBER,
  TOKEN_STRING,

  TOKEN_LINE,

  TOKEN_ERROR,
  TOKEN_EOF
} TokenType;

typedef struct Token_s
{
  TokenType type;

  // The beginning of the token as an offset of characters in the source.
  int start;

  // The offset of the character immediately following the end of the token.
  int end;

  // The 1-based line where the token appears.
  int line;
} Token;

typedef struct
{
  VM* vm;

  const char* source;

  // The index in source of the beginning of the currently-being-lexed token.
  int tokenStart;

  // The position of the current character being lexed.
  int currentChar;

  // The 1-based line number of [currentChar].
  int currentLine;

  // The most recently lexed token.
  Token current;

  // The most recently consumed/advanced token.
  Token previous;

  // Non-zero if subsequent newline tokens should be discarded.
  int skipNewlines;

  // Non-zero if a syntax or compile error has occurred.
  int hasError;
} Parser;

typedef struct sCompiler
{
  Parser* parser;

  // The compiler for the block enclosing this one, or NULL if it's the
  // top level.
  struct sCompiler* parent;

  // The function being compiled.
  ObjFn* fn;
  int numCodes;

  // Symbol table for declared local variables in this block.
  SymbolTable locals;

  // Non-zero if the function being compiled is a method.
  int isMethod;
} Compiler;

// Initializes [compiler].
static void initCompiler(Compiler* compiler, Parser* parser,
                         Compiler* parent, int isMethod)
{
  compiler->parser = parser;
  compiler->parent = parent;
  compiler->numCodes = 0;
  compiler->isMethod = isMethod;

  initSymbolTable(&compiler->locals);

  compiler->fn = newFunction(parser->vm);
  compiler->fn->numConstants = 0;
}

// Outputs a compile or syntax error.
static void error(Compiler* compiler, const char* format, ...)
{
  compiler->parser->hasError = 1;
  fprintf(stderr, "[Line %d] Error on '", compiler->parser->previous.line);

  for (int i = compiler->parser->previous.start;
       i < compiler->parser->previous.end; i++)
  {
    putc(compiler->parser->source[i], stderr);
  }

  fprintf(stderr, "': ");

  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);

  fprintf(stderr, "\n");
}

// Lexing ----------------------------------------------------------------------

// Returns non-zero if [c] is a valid (non-initial) identifier character.
static int isName(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Returns non-zero if [c] is a digit.
static int isDigit(char c)
{
  return c >= '0' && c <= '9';
}

// Returns the current character the parser is sitting on.
static char peekChar(Parser* parser)
{
  return parser->source[parser->currentChar];
}

// Returns the character after the current character.
static char peekNextChar(Parser* parser)
{
  // If we're at the end of the source, don't read past it.
  if (peekChar(parser) == '\0') return '\0';
  return parser->source[parser->currentChar + 1];
}

// Advances the parser forward one character.
static char nextChar(Parser* parser)
{
  char c = peekChar(parser);
  parser->currentChar++;
  return c;
}

// Sets the parser's current token to the given [type] and current character
// range.
static void makeToken(Parser* parser, TokenType type)
{
  parser->current.type = type;
  parser->current.start = parser->tokenStart;
  parser->current.end = parser->currentChar;
  parser->current.line = parser->currentLine;
}

// Skips the rest of the current line.
static void skipLineComment(Parser* parser)
{
  while (peekChar(parser) != '\n' && peekChar(parser) != '\0')
  {
    nextChar(parser);
  }
}

// Skips the rest of a block comment.
static void skipBlockComment(Parser* parser)
{
  nextChar(parser); // The opening "*".

  int nesting = 1;
  while (nesting > 0)
  {
    // TODO(bob): Unterminated comment. Should return error.
    if (peekChar(parser) == '\0') return;

    if (peekChar(parser) == '/' && peekNextChar(parser) == '*')
    {
      nextChar(parser);
      nextChar(parser);
      nesting++;
      continue;
    }

    if (peekChar(parser) == '*' && peekNextChar(parser) == '/')
    {
      nextChar(parser);
      nextChar(parser);
      nesting--;
      continue;
    }

    // Regular comment character.
    nextChar(parser);
  }
}

// Skips forward until a non-whitespace character is reached.
static void skipWhitespace(Parser* parser)
{
  while (peekChar(parser) == ' ') nextChar(parser);
}

// Returns non-zero if the current token's text matches [keyword].
static int isKeyword(Parser* parser, const char* keyword)
{
  size_t length = parser->currentChar - parser->tokenStart;
  size_t keywordLength = strlen(keyword);
  return length == keywordLength &&
  strncmp(parser->source + parser->tokenStart, keyword, length) == 0;
}

// Finishes lexing a number literal.
static void readNumber(Parser* parser)
{
  // TODO(bob): Hex, scientific, etc.
  while (isDigit(peekChar(parser))) nextChar(parser);

  // See if it has a floating point. Make sure there is a digit after the "."
  // so we don't get confused by method calls on number literals.
  if (peekChar(parser) == '.' && isDigit(peekNextChar(parser)))
  {
    nextChar(parser);
    while (isDigit(peekChar(parser))) nextChar(parser);
  }

  makeToken(parser, TOKEN_NUMBER);
}

// Finishes lexing an identifier. Handles reserved words.
static void readName(Parser* parser)
{
  // TODO(bob): Handle EOF.
  while (isName(peekChar(parser)) || isDigit(peekChar(parser)))
  {
    nextChar(parser);
  }

  TokenType type = TOKEN_NAME;

  if (isKeyword(parser, "class")) type = TOKEN_CLASS;
  if (isKeyword(parser, "else")) type = TOKEN_ELSE;
  if (isKeyword(parser, "false")) type = TOKEN_FALSE;
  if (isKeyword(parser, "fn")) type = TOKEN_FN;
  if (isKeyword(parser, "if")) type = TOKEN_IF;
  if (isKeyword(parser, "is")) type = TOKEN_IS;
  if (isKeyword(parser, "null")) type = TOKEN_NULL;
  if (isKeyword(parser, "static")) type = TOKEN_STATIC;
  if (isKeyword(parser, "this")) type = TOKEN_THIS;
  if (isKeyword(parser, "true")) type = TOKEN_TRUE;
  if (isKeyword(parser, "var")) type = TOKEN_VAR;

  makeToken(parser, type);
}

// Finishes lexing a string literal.
static void readString(Parser* parser)
{
  // TODO(bob): Escape sequences, EOL, EOF, etc.
  while (nextChar(parser) != '"');

  makeToken(parser, TOKEN_STRING);
}

// Lex the next token and store it in parser.current. Does not do any newline
// filtering.
static void readRawToken(Parser* parser)
{
  while (peekChar(parser) != '\0')
  {
    parser->tokenStart = parser->currentChar;

    char c = nextChar(parser);
    switch (c)
    {
      case '(': makeToken(parser, TOKEN_LEFT_PAREN); return;
      case ')': makeToken(parser, TOKEN_RIGHT_PAREN); return;
      case '[': makeToken(parser, TOKEN_LEFT_BRACKET); return;
      case ']': makeToken(parser, TOKEN_RIGHT_BRACKET); return;
      case '{': makeToken(parser, TOKEN_LEFT_BRACE); return;
      case '}': makeToken(parser, TOKEN_RIGHT_BRACE); return;
      case ':': makeToken(parser, TOKEN_COLON); return;
      case '.': makeToken(parser, TOKEN_DOT); return;
      case ',': makeToken(parser, TOKEN_COMMA); return;
      case '*': makeToken(parser, TOKEN_STAR); return;
      case '/':
        if (peekChar(parser) == '/')
        {
          skipLineComment(parser);
          break;
        }
        else if (peekChar(parser) == '*')
        {
          skipBlockComment(parser);
          break;
        }
        makeToken(parser, TOKEN_SLASH);
        return;

      case '%': makeToken(parser, TOKEN_PERCENT); return;
      case '+': makeToken(parser, TOKEN_PLUS); return;
      case '-':
        if (isDigit(peekChar(parser)))
        {
          readNumber(parser);
        }
        else
        {
          makeToken(parser, TOKEN_MINUS);
        }
        return;

      case '|': makeToken(parser, TOKEN_PIPE); return;
      case '&': makeToken(parser, TOKEN_AMP); return;
      case '=':
        if (peekChar(parser) == '=')
        {
          nextChar(parser);
          makeToken(parser, TOKEN_EQEQ);
        }
        else
        {
          makeToken(parser, TOKEN_EQ);
        }
        return;

      case '<':
        if (peekChar(parser) == '=')
        {
          nextChar(parser);
          makeToken(parser, TOKEN_LTEQ);
        }
        else
        {
          makeToken(parser, TOKEN_LT);
        }
        return;

      case '>':
        if (peekChar(parser) == '=')
        {
          nextChar(parser);
          makeToken(parser, TOKEN_GTEQ);
        }
        else
        {
          makeToken(parser, TOKEN_GT);
        }
        return;

      case '!':
        if (peekChar(parser) == '=')
        {
          nextChar(parser);
          makeToken(parser, TOKEN_BANGEQ);
        }
        else
        {
          makeToken(parser, TOKEN_BANG);
        }
        return;

      case '\n':
        parser->currentLine++;
        makeToken(parser, TOKEN_LINE);
        return;

      case ' ': skipWhitespace(parser); break;
      case '"': readString(parser); return;

      default:
        if (isName(c))
        {
          readName(parser);
        }
        else if (isDigit(c))
        {
          readNumber(parser);
        }
        else
        {
          // TODO(bob): Handle error.
          makeToken(parser, TOKEN_ERROR);
        }
        return;
    }
  }

  // If we get here, we're out of source, so just make EOF tokens.
  parser->tokenStart = parser->currentChar;
  makeToken(parser, TOKEN_EOF);
}

// Lex the next token in the source file and store it in parser.current. Omits
// newlines that aren't meaningful.
static void nextToken(Parser* parser)
{
  // TODO(bob): Check for EOF.
  parser->previous = parser->current;

  for (;;)
  {
    readRawToken(parser);

    switch (parser->current.type)
    {
      case TOKEN_LINE:
        if (!parser->skipNewlines)
        {
          // Collapse multiple newlines into one.
          parser->skipNewlines = 1;

          // Emit this newline.
          return;
        }
        break;

        // Discard newlines after tokens that cannot end an expression.
      case TOKEN_LEFT_PAREN:
      case TOKEN_LEFT_BRACKET:
      case TOKEN_LEFT_BRACE:
      case TOKEN_DOT:
      case TOKEN_COMMA:
      case TOKEN_STAR:
      case TOKEN_SLASH:
      case TOKEN_PERCENT:
      case TOKEN_PLUS:
      case TOKEN_MINUS:
      case TOKEN_PIPE:
      case TOKEN_AMP:
      case TOKEN_BANG:
      case TOKEN_EQ:
      case TOKEN_LT:
      case TOKEN_GT:
      case TOKEN_LTEQ:
      case TOKEN_GTEQ:
      case TOKEN_EQEQ:
      case TOKEN_BANGEQ:
      case TOKEN_CLASS:
      case TOKEN_ELSE:
      case TOKEN_IF:
      case TOKEN_IS:
      case TOKEN_STATIC:
      case TOKEN_VAR:
        parser->skipNewlines = 1;

        // Emit this token.
        return;

        // Newlines are meaningful after other tokens.
      default:
        parser->skipNewlines = 0;
        return;
    }
  }
}

// Parsing ---------------------------------------------------------------------

// Returns the type of the current token.
static TokenType peek(Compiler* compiler)
{
  return compiler->parser->current.type;
}

// Consumes the current token if its type is [expected]. Returns non-zero if a
// token was consumed.
static int match(Compiler* compiler, TokenType expected)
{
  if (peek(compiler) != expected) return 0;

  nextToken(compiler->parser);
  return 1;
}

// Consumes the current token. Emits an error if its type is not [expected].
static void consume(Compiler* compiler, TokenType expected,
                    const char* errorMessage)
{
  nextToken(compiler->parser);
  if (compiler->parser->previous.type != expected)
  {
    // TODO(bob): Better error.
    error(compiler, errorMessage);
  }
}

// Code generation utilities ---------------------------------------------------

// Emits one bytecode instruction or argument.
static int emit(Compiler* compiler, Code code)
{
  compiler->fn->bytecode[compiler->numCodes++] = code;
  return compiler->numCodes - 1;
}

// Adds [constant] to the constant pool and returns its index.
static int addConstant(Compiler* compiler, Value constant)
{
  compiler->fn->constants[compiler->fn->numConstants++] = constant;
  return compiler->fn->numConstants - 1;
}

// Parses a name token and declares a variable in the current scope with that
// name. Returns its symbol.
static int declareVariable(Compiler* compiler)
{
  consume(compiler, TOKEN_NAME, "Expected variable name.");

  SymbolTable* symbols;
  if (compiler->parent)
  {
    // Nested block, so this is a local variable.
    symbols = &compiler->locals;
  }
  else
  {
    // Top level global variable.
    symbols = &compiler->parser->vm->globalSymbols;
  }

  int symbol = addSymbol(symbols,
      compiler->parser->source + compiler->parser->previous.start,
      compiler->parser->previous.end - compiler->parser->previous.start);

  if (symbol == -1)
  {
    error(compiler, "Variable is already defined.");
  }

  return symbol;
}

// Stores a variable with the previously defined symbol in the current scope.
static void defineVariable(Compiler* compiler, int symbol)
{
  // If it's a global variable, we need to explicitly store it. If it's a local
  // variable, the value is already on the stack in the right slot.
  if (!compiler->parent)
  {
    // It's a global variable, so store the value in the global slot.
    emit(compiler, CODE_STORE_GLOBAL);
    emit(compiler, symbol);
  }
  else
  {
    // It's a local variable. The value is already in the right slot to store
    // the local, but later code will pop and discard that. To cancel that out
    // duplicate it now, so that the temporary value will be discarded and
    // leave the local still on the stack.
    // TODO(bob): Since variables are declared in statement position, this
    // generates a lot of code like:
    //
    //   var a = "value"
    //   io.write(a)
    //
    //   CODE_CONSTANT "value" // put constant into local slot
    //   CODE_DUP              // dup it so the top is a temporary
    //   CODE_POP              // discard previous result in sequence
    //   <code for io.write...>
    //
    // Would be good to either peephole optimize this or be smarted about
    // generating code for defining local variables to not emit the DUP
    // sometimes.
    emit(compiler, CODE_DUP);
  }
}

// Grammar ---------------------------------------------------------------------

typedef enum
{
  PREC_NONE,
  PREC_LOWEST,
  PREC_ASSIGNMENT, // =
  PREC_IS,         // is
  PREC_EQUALITY,   // == !=
  PREC_COMPARISON, // < > <= >=
  PREC_BITWISE,    // | &
  PREC_TERM,       // + -
  PREC_FACTOR,     // * / %
  PREC_UNARY,      // unary - ! ~
  PREC_CALL        // . ()
} Precedence;

// Forward declarations since the grammar is recursive.
static void expression(Compiler* compiler, int allowAssignment);
static void statement(Compiler* compiler);
static void definition(Compiler* compiler);
static void parsePrecedence(Compiler* compiler, int allowAssignment,
                            Precedence precedence);

typedef void (*GrammarFn)(Compiler*, int allowAssignment);

typedef void (*SignatureFn)(Compiler* compiler, char* name, int* length);

typedef struct
{
  GrammarFn prefix;
  GrammarFn infix;
  SignatureFn method;
  Precedence precedence;
  const char* name;
} GrammarRule;

GrammarRule rules[];

static void parameterList(Compiler* compiler, char* name, int* length)
{
  // Parse the parameter list, if any.
  if (match(compiler, TOKEN_LEFT_PAREN))
  {
    do
    {
      // Define a local variable in the method for the parameter.
      declareVariable(compiler);

      // Add a space in the name for the parameter.
      if (name != NULL) name[(*length)++] = ' ';
      // TODO(bob): Check for length overflow.
    }
    while (match(compiler, TOKEN_COMMA));
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  }
}

static void grouping(Compiler* compiler, int allowAssignment)
{
  expression(compiler, 0);
  consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

// Unary operators like `-foo`.
static void unaryOp(Compiler* compiler, int allowAssignment)
{
  GrammarRule* rule = &rules[compiler->parser->previous.type];

  // Compile the argument.
  parsePrecedence(compiler, 0, PREC_UNARY + 1);

  // Call the operator method on the left-hand side.
  int symbol = ensureSymbol(&compiler->parser->vm->methods, rule->name, 1);
  emit(compiler, CODE_CALL_0);
  emit(compiler, symbol);
}

static void boolean(Compiler* compiler, int allowAssignment)
{
  if (compiler->parser->previous.type == TOKEN_FALSE)
  {
    emit(compiler, CODE_FALSE);
  }
  else
  {
    emit(compiler, CODE_TRUE);
  }
}

static void function(Compiler* compiler, int allowAssignment)
{
  // TODO(bob): Copied from compileFunction(). Unify?
  Compiler fnCompiler;
  initCompiler(&fnCompiler, compiler->parser, compiler, 0);

  // Add the function to the constant table. Do this immediately so that it's
  // reachable by the GC.
  compiler->fn->constants[compiler->fn->numConstants++] = OBJ_VAL(fnCompiler.fn);

  // TODO(bob): Hackish.
  // Define a fake local slot for the receiver (the function object itself) so
  // that later locals have the correct slot indices.
  addSymbol(&fnCompiler.locals, "(this)", 6);

  parameterList(&fnCompiler, NULL, NULL);

  if (match(&fnCompiler, TOKEN_LEFT_BRACE))
  {
    // Block body.
    for (;;)
    {
      definition(&fnCompiler);

      // If there is no newline, it must be the end of the block on the same line.
      if (!match(&fnCompiler, TOKEN_LINE))
      {
        consume(&fnCompiler, TOKEN_RIGHT_BRACE,
                "Expect '}' after function body.");
        break;
      }

      if (match(&fnCompiler, TOKEN_RIGHT_BRACE)) break;

      // Discard the result of the previous expression.
      emit(&fnCompiler, CODE_POP);
    }
  }
  else
  {
    // Single expression body.
    // TODO(bob): Allow assignment here?
    expression(&fnCompiler, 0);
  }

  emit(&fnCompiler, CODE_END);

  // Compile the code to load it.
  emit(compiler, CODE_CONSTANT);
  emit(compiler, compiler->fn->numConstants - 1);
}

static void name(Compiler* compiler, int allowAssignment)
{
  // See if it's a local in this scope.
  int local = findSymbol(&compiler->locals,
      compiler->parser->source + compiler->parser->previous.start,
      compiler->parser->previous.end - compiler->parser->previous.start);

  // TODO(bob): Look up names in outer local scopes.

  // See if it's a global variable.
  int global = 0;
  if (local == -1)
  {
    global = findSymbol(&compiler->parser->vm->globalSymbols,
        compiler->parser->source + compiler->parser->previous.start,
        compiler->parser->previous.end - compiler->parser->previous.start);
  }

  // TODO(bob): Look for names in outer scopes.
  if (local == -1 && global == -1)
  {
    error(compiler, "Undefined variable.");
  }

  // If there's an "=" after a bare name, it's a variable assignment.
  if (match(compiler, TOKEN_EQ))
  {
    if (!allowAssignment) error(compiler, "Invalid assignment.");

    // Compile the right-hand side.
    statement(compiler);

    if (local != -1)
    {
      emit(compiler, CODE_STORE_LOCAL);
      emit(compiler, local);
      return;
    }

    emit(compiler, CODE_STORE_GLOBAL);
    emit(compiler, global);
    return;
  }

  // Otherwise, it's just a variable access.
  if (local != -1)
  {
    emit(compiler, CODE_LOAD_LOCAL);
    emit(compiler, local);
    return;
  }

  emit(compiler, CODE_LOAD_GLOBAL);
  emit(compiler, global);
}

static void null(Compiler* compiler, int allowAssignment)
{
  emit(compiler, CODE_NULL);
}

static void number(Compiler* compiler, int allowAssignment)
{
  Token* token = &compiler->parser->previous;
  char* end;

  double value = strtod(compiler->parser->source + token->start, &end);
  // TODO(bob): Check errno == ERANGE here.
  if (end == compiler->parser->source + token->start)
  {
    error(compiler, "Invalid number literal.");
    value = 0;
  }

  // Define a constant for the literal.
  int constant = addConstant(compiler, NUM_VAL(value));

  // Compile the code to load the constant.
  emit(compiler, CODE_CONSTANT);
  emit(compiler, constant);
}

static void string(Compiler* compiler, int allowAssignment)
{
  Token* token = &compiler->parser->previous;

  // TODO(bob): Handle escaping.

  // Ignore the surrounding "".
  size_t length = token->end - token->start - 2;
  const char* start = compiler->parser->source + token->start + 1;

  // Define a constant for the literal.
  int constant = addConstant(compiler,
      (Value)newString(compiler->parser->vm, start, length));

  // Compile the code to load the constant.
  emit(compiler, CODE_CONSTANT);
  emit(compiler, constant);
}

static void this_(Compiler* compiler, int allowAssignment)
{
  // TODO(bob): Not exactly right. This doesn't handle *functions* at the top
  // level.
  // Walk up the parent chain to see if there is an enclosing method.
  Compiler* thisCompiler = compiler;
  int insideMethod = 0;
  while (thisCompiler != NULL)
  {
    if (thisCompiler->isMethod)
    {
      insideMethod = 1;
      break;
    }

    thisCompiler = thisCompiler->parent;
  }

  if (!insideMethod)
  {
    error(compiler, "Cannot use 'this' outside of a method.");
    return;
  }

  // The receiver is always stored in the first local slot.
  // TODO(bob): Will need to do something different to handle functions
  // enclosed in methods.
  emit(compiler, CODE_LOAD_LOCAL);
  emit(compiler, 0);
}

void call(Compiler* compiler, int allowAssignment)
{
  char name[MAX_NAME];
  int length = 0;
  int numArgs = 0;

  consume(compiler, TOKEN_NAME, "Expect method name after '.'.");

  // Build the method name. To allow overloading by arity, we add a space to
  // the name for each argument.
  int partLength = compiler->parser->previous.end -
  compiler->parser->previous.start;
  strncpy(name + length,
          compiler->parser->source + compiler->parser->previous.start,
          partLength);
  length += partLength;
  // TODO(bob): Check for length overflow.

  // TODO(bob): Check for "=" here and set assignment and return.

  // Parse the argument list, if any.
  if (match(compiler, TOKEN_LEFT_PAREN))
  {
    do
    {
      statement(compiler);

      // Add a space in the name for each argument. Lets us overload by
      // arity.
      numArgs++;
      name[length++] = ' ';
    }
    while (match(compiler, TOKEN_COMMA));
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  }

  int symbol = ensureSymbol(&compiler->parser->vm->methods, name, length);

  // Compile the method call.
  emit(compiler, CODE_CALL_0 + numArgs);
  // TODO(bob): Handle > 10 args.
  emit(compiler, symbol);
}

void is(Compiler* compiler, int allowAssignment)
{
  // Compile the right-hand side.
  parsePrecedence(compiler, 0, PREC_CALL);

  emit(compiler, CODE_IS);
}

void infixOp(Compiler* compiler, int allowAssignment)
{
  GrammarRule* rule = &rules[compiler->parser->previous.type];

  // Compile the right-hand side.
  parsePrecedence(compiler, 0, rule->precedence + 1);

  // Call the operator method on the left-hand side.
  int symbol = ensureSymbol(&compiler->parser->vm->methods,
                            rule->name, strlen(rule->name));
  emit(compiler, CODE_CALL_1);
  emit(compiler, symbol);
}

// Compiles a method signature for an infix operator.
void infixSignature(Compiler* compiler, char* name, int* length)
{
  // Add a space for the RHS parameter.
  name[(*length)++] = ' ';

  // Parse the parameter name.
  declareVariable(compiler);
}

// Compiles a method signature for an unary operator (i.e. "!").
void unarySignature(Compiler* compiler, char* name, int* length)
{
  // Do nothing. The name is already complete.
}

// Compiles a method signature for an operator that can either be unary or
// infix (i.e. "-").
void mixedSignature(Compiler* compiler, char* name, int* length)
{
  // If there is a parameter name, it's an infix operator, otherwise it's unary.
  if (compiler->parser->current.type == TOKEN_NAME)
  {
    // Add a space for the RHS parameter.
    name[(*length)++] = ' ';

    // Parse the parameter name.
    declareVariable(compiler);
  }
}

// This table defines all of the parsing rules for the prefix and infix
// expressions in the grammar. Expressions are parsed using a Pratt parser.
//
// See: http://journal.stuffwithstuff.com/2011/03/19/pratt-parsers-expression-parsing-made-easy/
#define UNUSED                     { NULL, NULL, NULL, PREC_NONE, NULL }
#define PREFIX(fn)                 { fn, NULL, NULL, PREC_NONE, NULL }
#define INFIX(prec, fn)            { NULL, fn, NULL, prec, NULL }
#define INFIX_OPERATOR(prec, name) { NULL, infixOp, infixSignature, prec, name }
#define OPERATOR(prec, name)       { unaryOp, infixOp, mixedSignature, prec, name }
#define PREFIX_OPERATOR(name)      { unaryOp, NULL, unarySignature, PREC_NONE, name }

GrammarRule rules[] =
{
  /* TOKEN_LEFT_PAREN    */ PREFIX(grouping),
  /* TOKEN_RIGHT_PAREN   */ UNUSED,
  /* TOKEN_LEFT_BRACKET  */ UNUSED,
  /* TOKEN_RIGHT_BRACKET */ UNUSED,
  /* TOKEN_LEFT_BRACE    */ UNUSED,
  /* TOKEN_RIGHT_BRACE   */ UNUSED,
  /* TOKEN_COLON         */ UNUSED,
  /* TOKEN_DOT           */ INFIX(PREC_CALL, call),
  /* TOKEN_COMMA         */ UNUSED,
  /* TOKEN_STAR          */ INFIX_OPERATOR(PREC_FACTOR, "* "),
  /* TOKEN_SLASH         */ INFIX_OPERATOR(PREC_FACTOR, "/ "),
  /* TOKEN_PERCENT       */ INFIX_OPERATOR(PREC_TERM, "% "),
  /* TOKEN_PLUS          */ INFIX_OPERATOR(PREC_TERM, "+ "),
  /* TOKEN_MINUS         */ OPERATOR(PREC_TERM, "- "),
  /* TOKEN_PIPE          */ UNUSED,
  /* TOKEN_AMP           */ UNUSED,
  /* TOKEN_BANG          */ PREFIX_OPERATOR("!"),
  /* TOKEN_EQ            */ UNUSED,
  /* TOKEN_LT            */ INFIX_OPERATOR(PREC_COMPARISON, "< "),
  /* TOKEN_GT            */ INFIX_OPERATOR(PREC_COMPARISON, "> "),
  /* TOKEN_LTEQ          */ INFIX_OPERATOR(PREC_COMPARISON, "<= "),
  /* TOKEN_GTEQ          */ INFIX_OPERATOR(PREC_COMPARISON, ">= "),
  /* TOKEN_EQEQ          */ INFIX_OPERATOR(PREC_EQUALITY, "== "),
  /* TOKEN_BANGEQ        */ INFIX_OPERATOR(PREC_EQUALITY, "!= "),
  /* TOKEN_CLASS         */ UNUSED,
  /* TOKEN_ELSE          */ UNUSED,
  /* TOKEN_FALSE         */ PREFIX(boolean),
  /* TOKEN_FN            */ PREFIX(function),
  /* TOKEN_IF            */ UNUSED,
  /* TOKEN_IS            */ INFIX(PREC_IS, is),
  /* TOKEN_NULL          */ PREFIX(null),
  /* TOKEN_STATIC        */ UNUSED,
  /* TOKEN_THIS          */ PREFIX(this_),
  /* TOKEN_TRUE          */ PREFIX(boolean),
  /* TOKEN_VAR           */ UNUSED,
  /* TOKEN_NAME          */ { name, NULL, parameterList, PREC_NONE, NULL },
  /* TOKEN_NUMBER        */ PREFIX(number),
  /* TOKEN_STRING        */ PREFIX(string),
  /* TOKEN_LINE          */ UNUSED,
  /* TOKEN_ERROR         */ UNUSED,
  /* TOKEN_EOF           */ UNUSED
};

// The main entrypoint for the top-down operator precedence parser.
void parsePrecedence(Compiler* compiler, int allowAssignment,
                     Precedence precedence)
{
  nextToken(compiler->parser);
  GrammarFn prefix = rules[compiler->parser->previous.type].prefix;

  if (prefix == NULL)
  {
    // TODO(bob): Handle error better.
    error(compiler, "No prefix parser.");
    return;
  }

  prefix(compiler, allowAssignment);

  while (precedence <= rules[compiler->parser->current.type].precedence)
  {
    nextToken(compiler->parser);
    GrammarFn infix = rules[compiler->parser->previous.type].infix;
    infix(compiler, allowAssignment);
  }
}

// Parses an expression (or, really, the subset of expressions that can appear
// outside of the top level of a block). Does not include "statement-like"
// things like variable declarations.
void expression(Compiler* compiler, int allowAssignment)
{
  parsePrecedence(compiler, allowAssignment, PREC_LOWEST);
}

// Compiles an assignment expression.
void assignment(Compiler* compiler)
{
  // Assignment statement.
  expression(compiler, 1);
}

// Parses a "statement": any expression including expressions like variable
// declarations which can only appear at the top level of a block.
void statement(Compiler* compiler)
{
  // TODO(bob): Do we want to allow "if" in expression positions?
  if (match(compiler, TOKEN_IF))
  {
    // Compile the condition.
    consume(compiler, TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    assignment(compiler);
    consume(compiler, TOKEN_RIGHT_PAREN, "Expect ')' after if condition.");

    // Compile the then branch.
    emit(compiler, CODE_JUMP_IF);

    // Emit a placeholder. We'll patch it when we know what to jump to.
    int ifJump = emit(compiler, 255);

    statement(compiler);

    // Jump over the else branch when the if branch is taken.
    emit(compiler, CODE_JUMP);

    // Emit a placeholder. We'll patch it when we know what to jump to.
    int elseJump = emit(compiler, 255);

    // Patch the jump.
    compiler->fn->bytecode[ifJump] = compiler->numCodes - ifJump - 1;

    // Compile the else branch if there is one.
    if (match(compiler, TOKEN_ELSE))
    {
      statement(compiler);
    }
    else
    {
      // Just default to null.
      emit(compiler, CODE_NULL);
    }

    // Patch the jump over the else.
    compiler->fn->bytecode[elseJump] = compiler->numCodes - elseJump - 1;
    return;
  }

  // Curly block.
  if (match(compiler, TOKEN_LEFT_BRACE))
  {
    // TODO(bob): This code is duplicated in fn, method and top level parsing.
    for (;;)
    {
      // TODO(bob): Create a local variable scope.
      definition(compiler);

      // If there is no newline, it must be the end of the block on the same line.
      if (!match(compiler, TOKEN_LINE))
      {
        consume(compiler, TOKEN_RIGHT_BRACE, "Expect '}' after block body.");
        break;
      }

      if (match(compiler, TOKEN_RIGHT_BRACE)) break;

      // Discard the result of the previous expression.
      emit(compiler, CODE_POP);
    }
    return;
  }

  assignment(compiler);
}

// Compiles a method definition inside a class body.
void method(Compiler* compiler, int isStatic, SignatureFn signature)
{
  // TODO(bob): Copied from compileFunction(). Unify.
  Compiler methodCompiler;
  initCompiler(&methodCompiler, compiler->parser, compiler, 1);

  // Add the block to the constant table. Do this eagerly so it's reachable by
  // the GC.
  int constant = addConstant(compiler, OBJ_VAL(methodCompiler.fn));

  // TODO(bob): Hackish.
  // Define a fake local slot for the receiver so that later locals have the
  // correct slot indices.
  addSymbol(&methodCompiler.locals, "(this)", 6);

  // Build the method name.
  char name[MAX_NAME];
  int length = compiler->parser->previous.end -
               compiler->parser->previous.start;
  strncpy(name, compiler->parser->source + compiler->parser->previous.start,
          length);

  // Compile the method signature.
  signature(&methodCompiler, name, &length);

  int symbol = ensureSymbol(&compiler->parser->vm->methods, name, length);

  consume(compiler, TOKEN_LEFT_BRACE, "Expect '{' to begin method body.");

  // Block body.
  for (;;)
  {
    definition(&methodCompiler);

    // If there is no newline, it must be the end of the block on the same line.
    if (!match(&methodCompiler, TOKEN_LINE))
    {
      consume(&methodCompiler, TOKEN_RIGHT_BRACE, "Expect '}' after method body.");
      break;
    }

    if (match(&methodCompiler, TOKEN_RIGHT_BRACE)) break;

    // Discard the result of the previous expression.
    emit(&methodCompiler, CODE_POP);
  }

  emit(&methodCompiler, CODE_END);

  if (isStatic) emit(compiler, CODE_METACLASS);

  // Compile the code to define the method it.
  emit(compiler, CODE_METHOD);
  emit(compiler, symbol);
  emit(compiler, constant);

  if (isStatic)
  {
    emit(compiler, CODE_POP);
  }
}

// Compiles a name-binding statement.
void definition(Compiler* compiler)
{
  if (match(compiler, TOKEN_CLASS))
  {
    // Create a variable to store the class in.
    int symbol = declareVariable(compiler);

    // Load the superclass (if there is one).
    if (match(compiler, TOKEN_IS))
    {
      parsePrecedence(compiler, 0, PREC_CALL);
      emit(compiler, CODE_SUBCLASS);
    }
    else
    {
      // Create the empty class.
      emit(compiler, CODE_CLASS);
    }

    // Store it in its name.
    defineVariable(compiler, symbol);

    // Compile the method definitions.
    consume(compiler, TOKEN_LEFT_BRACE, "Expect '}' after class body.");

    while (!match(compiler, TOKEN_RIGHT_BRACE))
    {
      int isStatic = match(compiler, TOKEN_STATIC);

      SignatureFn signature = rules[compiler->parser->current.type].method;
      nextToken(compiler->parser);

      if (signature == NULL)
      {
        error(compiler, "Expect method definition.");
        break;
      }

      method(compiler, isStatic, signature);
      consume(compiler, TOKEN_LINE,
              "Expect newline after definition in class.");
    }

    return;
  }

  if (match(compiler, TOKEN_VAR))
  {
    int symbol = declareVariable(compiler);

    // TODO(bob): Allow uninitialized vars?
    consume(compiler, TOKEN_EQ, "Expect '=' after variable name.");

    // Compile the initializer.
    statement(compiler);

    defineVariable(compiler, symbol);
    return;
  }

  statement(compiler);
}

// Parses [source] to a "function" (a chunk of top-level code) for execution by
// [vm].
ObjFn* compile(VM* vm, const char* source)
{
  Parser parser;
  parser.vm = vm;
  parser.source = source;
  parser.hasError = 0;

  // Ignore leading newlines.
  parser.skipNewlines = 1;

  parser.tokenStart = 0;
  parser.currentChar = 0;
  parser.currentLine = 1;

  // Zero-init the current token. This will get copied to previous when
  // advance() is called below.
  parser.current.type = TOKEN_EOF;
  parser.current.start = 0;
  parser.current.end = 0;
  parser.current.line = 0;

  // Read the first token.
  nextToken(&parser);

  Compiler compiler;
  initCompiler(&compiler, &parser, NULL, 0);

  pinObj(vm, (Obj*)compiler.fn);

  for (;;)
  {
    definition(&compiler);

    // If there is no newline, it must be the end of the block on the same line.
    if (!match(&compiler, TOKEN_LINE))
    {
      consume(&compiler, TOKEN_EOF, "Expect end of file.");
      break;
    }

    if (match(&compiler, TOKEN_EOF)) break;

    // Discard the result of the previous expression.
    emit(&compiler, CODE_POP);
  }

  emit(&compiler, CODE_END);

  unpinObj(vm, (Obj*)compiler.fn);

  return parser.hasError ? NULL : compiler.fn;
}
