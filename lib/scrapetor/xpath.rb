# frozen_string_literal: true

module Scrapetor
  # Full XPath 1.0 expression engine.
  #
  # Pipeline:
  #   1. Tokenizer  -> array of [:type, value] tokens
  #   2. Parser     -> AST (recursive-descent, full XPath 1.0 grammar)
  #   3. Evaluator  -> walks the AST against a Scrapetor::Document/Node
  #
  # Axis traversals dispatch to native C primitives on the arena DOM
  # (`node_following_sibling_ids`, `node_ancestor_ids`, `node_following_ids`,
  # `node_preceding_ids`, `node_descendant_comment_ids`, …) so the hot
  # path stays in C even though the AST walk runs in Ruby.
  #
  # Compiled ASTs are cached on the module (LRU-bounded) so repeated
  # queries — typical in scraping pipelines that run the same parser
  # against thousands of pages — only pay the tokenize/parse cost once.
  module XPath
    class UnsupportedError < StandardError; end
    class ParseError       < StandardError; end

    AST_CACHE_CAP = 1024
    @ast_cache = {}
    @ast_cache_mutex = Mutex.new

    def self.evaluate(context, expr)
      expr_s = expr.to_s
      # Memo the AST + CSS-translation result together so the per-call
      # overhead on the hot path collapses to one Hash lookup. The first
      # call for a new expression pays parse + translate; every later
      # call gets the cached descriptor or `false` (= no CSS fast path).
      entry = @ast_cache[expr_s] || cache_compile(expr_s)
      if (css = entry[:css])
        return run_via_css(context, css)
      end
      Evaluator.new(context).eval_program(entry[:ast])
    end

    def self.compile(expr)
      cache_compile(expr.to_s)[:ast]
    end

    def self.cache_compile(expr)
      cached = @ast_cache[expr]
      return cached if cached
      @ast_cache_mutex.synchronize do
        cached = @ast_cache[expr]
        return cached if cached
        ast = Parser.new(Tokenizer.tokenize(expr), expr).parse_expr
        css = CssTranslator.translate(ast)
        entry = { ast: ast, css: css }
        @ast_cache.shift if @ast_cache.size >= AST_CACHE_CAP
        @ast_cache[expr] = entry
        entry
      end
    end

    # Execute a translated CSS chain. Handles the ::attr / ::text
    # tail forms that CssTranslator emits for `/@x` and `/text()`
    # terminations. Returns an Array (XPath shape) regardless of
    # the underlying CSS return type.
    def self.run_via_css(context, css_descriptor)
      sel = css_descriptor[:sel]
      kind = css_descriptor[:kind] # :nodes / :attr / :text
      result = context.css(sel)
      arr = result.respond_to?(:to_a) ? result.to_a : Array(result)
      arr
    end

    # ============================================================
    # Tokenizer
    # ============================================================

    module Tokenizer
      OPERATORS = %w[// / .. . :: @ ( ) [ ] , | + - = != <= >= < > * div mod and or].freeze

      def self.tokenize(s)
        tokens = []
        i = 0
        len = s.length
        while i < len
          c = s[i]
          case c
          when " ", "\t", "\n", "\r"
            i += 1
          when "/"
            if s[i + 1] == "/"
              tokens << [:slash_slash, "//"]; i += 2
            else
              tokens << [:slash, "/"]; i += 1
            end
          when "("
            tokens << [:lparen, "("]; i += 1
          when ")"
            tokens << [:rparen, ")"]; i += 1
          when "["
            tokens << [:lbracket, "["]; i += 1
          when "]"
            tokens << [:rbracket, "]"]; i += 1
          when ","
            tokens << [:comma, ","]; i += 1
          when "@"
            tokens << [:at, "@"]; i += 1
          when "|"
            tokens << [:pipe, "|"]; i += 1
          when "+"
            tokens << [:plus, "+"]; i += 1
          when "-"
            # `-` is a tricky one. In XPath 1.0 it's only an operator
            # when the preceding token is one of: another operator, `(`,
            # `[`, `,`, or nothing (start of expression). Otherwise it's
            # part of a name. NameTest disambiguates downstream.
            prev = tokens.last
            if prev.nil? || %i[lparen lbracket comma op slash slash_slash pipe at plus minus eq neq lt gt le ge star and_op or_op].include?(prev[0])
              tokens << [:minus, "-"]
            else
              tokens << [:minus, "-"]
            end
            i += 1
          when "="
            tokens << [:eq, "="]; i += 1
          when "!"
            if s[i + 1] == "="
              tokens << [:neq, "!="]; i += 2
            else
              raise ParseError, "stray `!` in `#{s}`"
            end
          when "<"
            if s[i + 1] == "="
              tokens << [:le, "<="]; i += 2
            else
              tokens << [:lt, "<"]; i += 1
            end
          when ">"
            if s[i + 1] == "="
              tokens << [:ge, ">="]; i += 2
            else
              tokens << [:gt, ">"]; i += 1
            end
          when ":"
            if s[i + 1] == ":"
              tokens << [:axis_sep, "::"]; i += 2
            else
              tokens << [:colon, ":"]; i += 1
            end
          when "*"
            # `*` is multiplicative when prev is a value-producing token;
            # otherwise it's NameTest "any element".
            prev = tokens.last
            if prev && %i[name number string rparen rbracket dot at_attr_done].include?(prev[0])
              tokens << [:star_mul, "*"]
            else
              tokens << [:star, "*"]
            end
            i += 1
          when "."
            if s[i + 1] == "."
              tokens << [:dot_dot, ".."]; i += 2
            elsif s[i + 1] && (s[i + 1] >= "0" && s[i + 1] <= "9")
              # Numeric literal starting with .
              j = i + 1
              j += 1 while j < len && s[j] >= "0" && s[j] <= "9"
              tokens << [:number, s[i...j].to_f]; i = j
            else
              tokens << [:dot, "."]; i += 1
            end
          when "'", '"'
            quote = c
            j = i + 1
            j += 1 while j < len && s[j] != quote
            raise ParseError, "unterminated string in `#{s}`" if j >= len
            tokens << [:string, s[(i + 1)...j]]
            i = j + 1
          when "0".."9"
            j = i
            j += 1 while j < len && s[j] >= "0" && s[j] <= "9"
            if j < len && s[j] == "." && (j + 1 >= len || (s[j + 1] >= "0" && s[j + 1] <= "9"))
              j += 1
              j += 1 while j < len && s[j] >= "0" && s[j] <= "9"
              tokens << [:number, s[i...j].to_f]
            else
              tokens << [:number, s[i...j].to_i]
            end
            i = j
          else
            # Name token: NCName chars (letters, digits, _, -). XPath
            # operators `div`, `mod`, `and`, `or` are name-shaped; we
            # classify them post-hoc based on context.
            if c =~ /[A-Za-z_]/
              j = i
              j += 1 while j < len && s[j] =~ /[A-Za-z0-9_\-]/
              name = s[i...j]
              prev = tokens.last
              # Operator names only kick in when the prior token suggests
              # we're in an operator position (after a value-producing token).
              op_position = prev && %i[name number string rparen rbracket star_mul dot_dot dot].include?(prev[0])
              if op_position && name == "and"
                tokens << [:and_op, "and"]
              elsif op_position && name == "or"
                tokens << [:or_op, "or"]
              elsif op_position && name == "div"
                tokens << [:div_op, "div"]
              elsif op_position && name == "mod"
                tokens << [:mod_op, "mod"]
              else
                tokens << [:name, name]
              end
              i = j
            else
              raise ParseError, "unrecognised char `#{c}` at #{i} in `#{s}`"
            end
          end
        end
        tokens << [:eof, nil]
        tokens
      end
    end

    # ============================================================
    # Parser → AST
    #
    # AST node shapes (all Hashes):
    #   { t: :path,  steps: [step,...], absolute: bool, double_slash: bool }
    #   step: { axis: :child|:descendant_or_self|..., nt: nodetest, preds: [Expr,...] }
    #   nodetest: :any_element | :text | :comment | :node | :pi(name=...) | {name: 'tag'} | :attr({name})
    #   { t: :or,  l:, r: }
    #   { t: :and, l:, r: }
    #   { t: :cmp, op: :eq|:neq|:lt|:le|:gt|:ge, l:, r: }
    #   { t: :add, op: :plus|:minus, l:, r: }
    #   { t: :mul, op: :mul|:div|:mod, l:, r: }
    #   { t: :neg, e: }
    #   { t: :union, ops: [Expr,...] }
    #   { t: :filter, primary: Expr, preds: [Expr,...] } chained with /path
    #   { t: :func, name: 'count', args: [Expr,...] }
    #   { t: :num, v: }
    #   { t: :str, v: }
    # ============================================================

    class Parser
      def initialize(tokens, raw)
        @tokens = tokens
        @pos = 0
        @raw = raw
      end

      def peek(k = 0); @tokens[@pos + k]; end
      def consume; t = @tokens[@pos]; @pos += 1; t; end
      def expect(type)
        t = @tokens[@pos]
        unless t && t[0] == type
          raise ParseError, "expected #{type} got #{t.inspect} in `#{@raw}`"
        end
        @pos += 1
        t
      end

      def parse_expr
        parse_or
      end

      def parse_or
        l = parse_and
        while peek[0] == :or_op
          consume
          l = { t: :or, l: l, r: parse_and }
        end
        l
      end

      def parse_and
        l = parse_equality
        while peek[0] == :and_op
          consume
          l = { t: :and, l: l, r: parse_equality }
        end
        l
      end

      def parse_equality
        l = parse_relational
        while %i[eq neq].include?(peek[0])
          op = consume[0]
          l = { t: :cmp, op: op, l: l, r: parse_relational }
        end
        l
      end

      def parse_relational
        l = parse_additive
        while %i[lt le gt ge].include?(peek[0])
          op = consume[0]
          l = { t: :cmp, op: op, l: l, r: parse_additive }
        end
        l
      end

      def parse_additive
        l = parse_multiplicative
        while %i[plus minus].include?(peek[0])
          op = consume[0] == :plus ? :plus : :minus
          l = { t: :add, op: op, l: l, r: parse_multiplicative }
        end
        l
      end

      def parse_multiplicative
        l = parse_unary
        loop do
          tk = peek[0]
          break unless %i[star_mul div_op mod_op].include?(tk)
          consume
          op = tk == :star_mul ? :mul : (tk == :div_op ? :div : :mod)
          l = { t: :mul, op: op, l: l, r: parse_unary }
        end
        l
      end

      def parse_unary
        if peek[0] == :minus
          consume
          { t: :neg, e: parse_unary }
        else
          parse_union
        end
      end

      def parse_union
        l = parse_path
        if peek[0] == :pipe
          ops = [l]
          while peek[0] == :pipe
            consume
            ops << parse_path
          end
          { t: :union, ops: ops }
        else
          l
        end
      end

      # PathExpr := LocationPath | FilterExpr ('/' RelativeLocationPath | '//' RelativeLocationPath)?
      def parse_path
        if location_path_start?
          parse_location_path
        else
          primary = parse_primary
          # PrimaryExpr Predicate*
          preds = []
          while peek[0] == :lbracket
            consume
            preds << parse_expr
            expect(:rbracket)
          end
          base = preds.empty? ? primary : { t: :filter, primary: primary, preds: preds }
          # Optional /RelativeLocationPath or //RelativeLocationPath
          if %i[slash slash_slash].include?(peek[0])
            steps = []
            consume_path_separator(steps)
            parse_relative_location_path_into(steps)
            { t: :filter_path, primary: base, steps: steps }
          else
            base
          end
        end
      end

      def location_path_start?
        tk = peek[0]
        return true if %i[slash slash_slash dot dot_dot at star].include?(tk)
        # name token followed by something that looks like a node-test
        # (axis_sep, paren-without-args-as-function, slash, predicate)
        if tk == :name
          n2 = peek(1)[0]
          # `name::` is an axis
          return true if n2 == :axis_sep
          # `name(...)` -> function or nodetype()
          if n2 == :lparen
            # Distinguish nodetype() vs function call. Nodetypes are:
            # text, comment, node, processing-instruction.
            nm = peek[1]
            return %w[text comment node processing-instruction].include?(nm)
          end
          # bare name like `div` — that's a location step (child::div)
          return true
        end
        false
      end

      def parse_location_path
        steps = []
        absolute = false
        double = false
        if peek[0] == :slash_slash
          absolute = true; double = true
          steps << { axis: :descendant_or_self, nt: :node, preds: [] }
          consume
        elsif peek[0] == :slash
          absolute = true
          consume
          # If the next token doesn't start a step, this is just '/'.
          if !step_start?
            return { t: :path, steps: steps, absolute: absolute, double_slash: false }
          end
        end
        parse_relative_location_path_into(steps)
        { t: :path, steps: steps, absolute: absolute, double_slash: double }
      end

      def parse_relative_location_path_into(steps)
        steps << parse_step
        loop do
          break unless %i[slash slash_slash].include?(peek[0])
          consume_path_separator(steps)
          steps << parse_step
        end
      end

      def consume_path_separator(steps)
        tk = consume[0]
        if tk == :slash_slash
          steps << { axis: :descendant_or_self, nt: :node, preds: [] }
        end
      end

      def step_start?
        %i[name star at dot dot_dot].include?(peek[0])
      end

      AXIS_SYMBOLS = {
        "child"              => :child,
        "descendant"         => :descendant,
        "descendant-or-self" => :descendant_or_self,
        "parent"             => :parent,
        "self"               => :self,
        "ancestor"           => :ancestor,
        "ancestor-or-self"   => :ancestor_or_self,
        "following-sibling"  => :following_sibling,
        "preceding-sibling"  => :preceding_sibling,
        "following"          => :following,
        "preceding"          => :preceding,
        "attribute"          => :attribute,
        "namespace"          => :namespace
      }.freeze

      def parse_step
        if peek[0] == :dot
          consume
          return { axis: :self, nt: :node, preds: [] }
        end
        if peek[0] == :dot_dot
          consume
          return { axis: :parent, nt: :node, preds: [] }
        end

        axis = :child
        if peek[0] == :at
          consume
          axis = :attribute
        elsif peek[0] == :name && peek(1)[0] == :axis_sep
          name = consume[1]
          axis_sym = AXIS_SYMBOLS[name]
          raise UnsupportedError, "unknown axis `#{name}` in `#{@raw}`" unless axis_sym
          axis = axis_sym
          expect(:axis_sep)
        end

        nt = parse_node_test
        preds = []
        while peek[0] == :lbracket
          consume
          preds << parse_expr
          expect(:rbracket)
        end
        { axis: axis, nt: nt, preds: preds }
      end

      def parse_node_test
        if peek[0] == :star
          consume
          return :any_element
        end
        if peek[0] == :name && peek(1)[0] == :lparen
          nm = consume[1]
          expect(:lparen)
          case nm
          when "node"
            expect(:rparen)
            return :node
          when "text"
            expect(:rparen)
            return :text
          when "comment"
            expect(:rparen)
            return :comment
          when "processing-instruction"
            arg = nil
            if peek[0] == :string
              arg = consume[1]
            end
            expect(:rparen)
            return { pi: arg }
          else
            raise ParseError, "unexpected `#{nm}(` as node test in `#{@raw}`"
          end
        end
        if peek[0] == :name
          # tag name. Support qname (prefix:local) — we ignore the prefix.
          name = consume[1]
          if peek[0] == :colon && peek(1)[0] == :name
            consume
            name = consume[1]
          end
          return { name: name }
        end
        raise ParseError, "expected node test, got #{peek.inspect} in `#{@raw}`"
      end

      def parse_primary
        tk = peek[0]
        case tk
        when :string
          { t: :str, v: consume[1] }
        when :number
          { t: :num, v: consume[1] }
        when :lparen
          consume
          e = parse_expr
          expect(:rparen)
          e
        when :name
          # FunctionCall
          fname = consume[1]
          expect(:lparen)
          args = []
          unless peek[0] == :rparen
            args << parse_expr
            while peek[0] == :comma
              consume
              args << parse_expr
            end
          end
          expect(:rparen)
          { t: :func, name: fname, args: args }
        else
          raise ParseError, "unexpected `#{peek.inspect}` in `#{@raw}`"
        end
      end
    end

    # ============================================================
    # CSS Translator: convert simple XPath AST shapes to CSS selectors
    # so the heavily-optimised native CSS matcher answers them in one
    # C call. Returns nil if the AST contains anything that doesn't
    # round-trip cleanly to CSS (boolean predicates, position()/last()
    # functions, sibling/ancestor axes, etc.) — caller then falls back
    # to the full evaluator.
    # ============================================================

    module CssTranslator
      # Returns { sel: "...", kind: :nodes|:attr|:text } or nil.
      def self.translate(ast)
        return nil unless ast.is_a?(Hash) && ast[:t] == :path
        steps = ast[:steps]
        return nil if steps.empty?

        # We support these path patterns:
        #   absolute (/, //) and relative (.//, scoped from current node).
        # The leading `descendant-or-self any-element` step that // injects
        # gets collapsed with the next step: //tag becomes "tag", //a/b
        # becomes "a > b" only when an explicit child separator follows.
        idx = 0
        css_parts = []
        prev_was_descendant = ast[:absolute]
        # If absolute starts with a single `/`, the first real step is at
        # the document root child level → tighten with `> tag`. // (double_slash)
        # already injected a descendant-or-self step.

        while idx < steps.length
          st = steps[idx]
          axis = st[:axis]
          nt = st[:nt]
          preds = st[:preds]

          # `descendant-or-self node()` (from //) — combiner only.
          if axis == :descendant_or_self && nt == :node && preds.empty?
            prev_was_descendant = true
            idx += 1
            next
          end

          # Tail extractions: @attr and text() must be the final step.
          last = idx == steps.length - 1
          if axis == :attribute && nt.is_a?(Hash) && nt[:name] && last
            base = css_parts.join
            return nil if base.empty?
            return { sel: "#{base}::attr(#{nt[:name]})", kind: :attr }
          end
          # XPath text() returns one TextNode per literal text segment;
          # CSS `::text` concatenates a node's textContent. The
          # semantics diverge whenever an element has mixed text+inline
          # children, so we never route text() through CSS — the full
          # evaluator walks the arena and emits separate TextNodes per
          # text node id, which matches XPath / Nokogiri semantics.

          # Following-sibling axis: CSS `~` (general sibling) when the
          # name test is concrete, equivalently `* + tag` for the [1]
          # case (adjacent sibling). XPath following-sibling::name and
          # CSS `~ name` both select siblings of the context node that
          # come after it and match name, regardless of intervening
          # nodes — identical semantics.
          if axis == :following_sibling
            tag = sibling_axis_tag(nt)
            return nil unless tag
            pred_strs = collect_pred_strs(preds)
            return nil if pred_strs.nil?
            return nil if css_parts.empty?
            css_parts << " ~ " << tag
            pred_strs.each { |ps| css_parts << ps }
            prev_was_descendant = false
            idx += 1
            next
          end

          # Only handle child axis for intermediate steps in the CSS path.
          return nil unless axis == :child

          # Node test must be a tag name or `*`.
          tag =
            case nt
            when :any_element then "*"
            when Hash
              return nil unless nt[:name]
              nt[:name]
            else
              return nil
            end

          # Predicates: translate each to CSS bracket / pseudo if possible.
          pred_strs = collect_pred_strs(preds)
          return nil if pred_strs.nil?

          if css_parts.empty?
            css_parts << tag
          elsif prev_was_descendant
            css_parts << " " << tag
          else
            css_parts << " > " << tag
          end
          pred_strs.each { |ps| css_parts << ps }

          prev_was_descendant = false
          idx += 1
        end

        sel = css_parts.join
        return nil if sel.empty?
        { sel: sel, kind: :nodes }
      end

      # Convert a predicate AST to a CSS bracket / pseudo selector
      # fragment. Returns nil if the predicate uses anything CSS can't
      # express (booleans, position()/last() functions, text() etc.).
      def self.translate_predicate(ast)
        case ast[:t]
        when :path
          # @attr alone: a path with one step axis=:attribute, nt={name:...}.
          steps = ast[:steps]
          return nil unless steps.length == 1
          st = steps[0]
          if st[:axis] == :attribute && st[:nt].is_a?(Hash) && st[:nt][:name] && st[:preds].empty?
            return "[#{st[:nt][:name]}]"
          end
          nil
        when :num
          # Positional predicate [N]: XPath `child::tag[N]` ≡ "Nth tag
          # child" which matches CSS `:nth-of-type(N)` exactly (both pick
          # the Nth member of the same-tag children of the parent).
          n = ast[:v].to_i
          return nil unless n >= 1
          ":nth-of-type(#{n})"
        when :cmp
          # position() comparisons: XPath position() refers to the
          # context position within the parent's same-tag children
          # (matching CSS :nth-of-type semantics). These translate to
          # the corresponding :nth-of-type formulas:
          #   position() = N    → :nth-of-type(N)
          #   position() > N    → :nth-of-type(n+N+1)
          #   position() >= N   → :nth-of-type(n+N)
          #   position() < N    → :nth-of-type(-n+N-1)
          #   position() <= N   → :nth-of-type(-n+N)
          if is_position_func?(ast[:l]) && (n = const_int(ast[:r]))
            return nth_for(ast[:op], n)
          elsif is_position_func?(ast[:r]) && (n = const_int(ast[:l]))
            return nth_for(flip_cmp(ast[:op]), n)
          end
          return nil unless ast[:op] == :eq
          attr_name = extract_attr_name(ast[:l])
          val_lit   = extract_string_literal(ast[:r])
          # try the other order
          if attr_name.nil?
            attr_name = extract_attr_name(ast[:r])
            val_lit   = extract_string_literal(ast[:l])
          end
          return nil if attr_name.nil? || val_lit.nil?
          "[#{attr_name}=#{quote_css(val_lit)}]"
        when :and
          # Boolean AND of two simpler predicates → just concatenate the
          # CSS fragments (CSS treats `[a][b]` as logical AND).
          l = translate_predicate(ast[:l])
          r = translate_predicate(ast[:r])
          return nil if l.nil? || r.nil?
          "#{l}#{r}"
        when :func
          case ast[:name]
          when "contains"
            a = extract_attr_name(ast[:args][0])
            v = extract_string_literal(ast[:args][1])
            return nil unless a && v
            "[#{a}*=#{quote_css(v)}]"
          when "starts-with"
            a = extract_attr_name(ast[:args][0])
            v = extract_string_literal(ast[:args][1])
            return nil unless a && v
            "[#{a}^=#{quote_css(v)}]"
          else
            nil
          end
        else
          nil
        end
      end

      def self.extract_attr_name(ast)
        return nil unless ast.is_a?(Hash) && ast[:t] == :path
        steps = ast[:steps]
        return nil unless steps.length == 1
        st = steps[0]
        return nil unless st[:axis] == :attribute && st[:nt].is_a?(Hash) && st[:nt][:name]
        st[:nt][:name]
      end

      def self.extract_string_literal(ast)
        return nil unless ast.is_a?(Hash)
        ast[:t] == :str ? ast[:v] : nil
      end

      def self.quote_css(s)
        s.match?(/[\s\[\]'"=]/) ? "\"#{s.gsub('"', '\\"')}\"" : "'#{s}'"
      end

      def self.sibling_axis_tag(nt)
        case nt
        when :any_element then "*"
        when Hash
          nt[:name]
        end
      end

      def self.collect_pred_strs(preds)
        out = []
        preds.each do |p|
          cs = translate_predicate(p)
          return nil if cs.nil?
          out << cs
        end
        out
      end

      def self.is_position_func?(ast)
        ast.is_a?(Hash) && ast[:t] == :func && ast[:name] == "position" && ast[:args].empty?
      end

      def self.const_int(ast)
        return nil unless ast.is_a?(Hash) && ast[:t] == :num
        n = ast[:v]
        n.respond_to?(:to_i) ? n.to_i : nil
      end

      def self.flip_cmp(op)
        # Flip the operator when operands are swapped
        case op
        when :lt then :gt
        when :le then :ge
        when :gt then :lt
        when :ge then :le
        else op
        end
      end

      def self.nth_for(op, n)
        case op
        when :eq then ":nth-of-type(#{n})"
        when :gt then ":nth-of-type(n+#{n + 1})"
        when :ge then ":nth-of-type(n+#{n})"
        when :lt
          # All positions strictly less than n. CSS `-n+N` matches 1..N.
          return nil if n <= 1
          ":nth-of-type(-n+#{n - 1})"
        when :le
          return nil if n < 1
          ":nth-of-type(-n+#{n})"
        end
      end
    end

    # ============================================================
    # Evaluator
    # ============================================================

    class Evaluator
      def initialize(context)
        @document = context.is_a?(Scrapetor::Document) ? context : context.document
        @native_doc, @native_root_id = native_handles_for(context)
        @context_input = context
      end

      def eval_program(ast)
        result = eval_expr(ast, [ initial_context_node ], nil)
        # Flatten singleton arrays produced by terminal extractions.
        result.is_a?(Array) ? result : [result]
      end

      def initial_context_node
        # For Document inputs: the context is the document wrapper (so we
        # can descend into its children via the arena). For Node inputs:
        # the context is the node itself.
        @initial =
          if @context_input.is_a?(Scrapetor::Document)
            @context_input.backing
          else
            @context_input
          end
      end

      # eval_expr returns one of:
      #   Array<Node|String>   (node-set or string-set for /@x and /text())
      #   String / Numeric / TrueClass / FalseClass / NilClass (scalar)
      def eval_expr(ast, context_set, position_info)
        case ast[:t]
        when :path
          eval_path(ast, context_set, position_info)
        when :filter
          base = eval_expr(ast[:primary], context_set, position_info)
          apply_predicates(base, ast[:preds])
        when :filter_path
          base = eval_expr(ast[:primary], context_set, position_info)
          eval_steps_against(base, ast[:steps])
        when :union
          out = []
          seen = {}
          ast[:ops].each do |op|
            r = eval_expr(op, context_set, position_info)
            r = [r] unless r.is_a?(Array)
            r.each do |n|
              key = node_identity(n)
              next if seen[key]
              seen[key] = true
              out << n
            end
          end
          out
        when :or
          xpath_boolean(eval_expr(ast[:l], context_set, position_info)) ||
            xpath_boolean(eval_expr(ast[:r], context_set, position_info))
        when :and
          xpath_boolean(eval_expr(ast[:l], context_set, position_info)) &&
            xpath_boolean(eval_expr(ast[:r], context_set, position_info))
        when :cmp
          do_compare(ast[:op],
                     eval_expr(ast[:l], context_set, position_info),
                     eval_expr(ast[:r], context_set, position_info))
        when :add
          l = xpath_number(eval_expr(ast[:l], context_set, position_info))
          r = xpath_number(eval_expr(ast[:r], context_set, position_info))
          return Float::NAN if l.respond_to?(:nan?) && (l.nan? || r.nan?)
          ast[:op] == :plus ? (l + r) : (l - r)
        when :mul
          l = xpath_number(eval_expr(ast[:l], context_set, position_info))
          r = xpath_number(eval_expr(ast[:r], context_set, position_info))
          case ast[:op]
          when :mul then l * r
          when :div
            r.zero? ? (l.zero? ? Float::NAN : (l.positive? ? Float::INFINITY : -Float::INFINITY)) : (l.to_f / r.to_f)
          when :mod
            r.zero? ? Float::NAN : (l - (l.to_i / r.to_i) * r)
          end
        when :neg
          -xpath_number(eval_expr(ast[:e], context_set, position_info))
        when :num
          ast[:v]
        when :str
          ast[:v]
        when :func
          call_function(ast[:name], ast[:args], context_set, position_info)
        else
          raise UnsupportedError, "unknown AST node: #{ast[:t]}"
        end
      end

      def eval_path(ast, context_set, position_info)
        nodes =
          if ast[:absolute]
            [ root_for_context ]
          else
            context_set
          end
        eval_steps_against(nodes, ast[:steps])
      end

      def eval_steps_against(nodes, steps)
        current = nodes
        steps.each do |st|
          current = step_walk(current, st)
          current = apply_step_predicates(current, st[:preds]) unless st[:preds].empty?
        end
        current
      end

      def step_walk(current, st)
        axis = st[:axis]
        nt = st[:nt]
        out = []
        current.each do |n|
          case axis
          when :child
            collect_children(n, nt, out)
          when :descendant
            collect_descendants(n, nt, out)
          when :descendant_or_self
            collect_self(n, nt, out)
            collect_descendants(n, nt, out)
          when :parent
            p = parent_of(n)
            push_if_matches(p, nt, out) if p
          when :self
            collect_self(n, nt, out)
          when :ancestor
            ancestors_of(n).each { |a| push_if_matches(a, nt, out) }
          when :ancestor_or_self
            ancestors_of(n).each { |a| push_if_matches(a, nt, out) }
            collect_self(n, nt, out)
          when :following_sibling
            following_siblings_of(n).each { |s| push_if_matches(s, nt, out) }
          when :preceding_sibling
            preceding_siblings_of(n).each { |s| push_if_matches(s, nt, out) }
          when :following
            following_of(n).each { |s| push_if_matches(s, nt, out) }
          when :preceding
            preceding_of(n).each { |s| push_if_matches(s, nt, out) }
          when :attribute
            collect_attributes(n, nt, out)
          when :namespace
            # No-op: we don't model namespace nodes.
          end
        end
        # Per XPath 1.0 §2.1: every axis step produces a node-set
        # (i.e. duplicate-free, document-ordered). When the input
        # context set has multiple nodes, the axis walks can produce
        # overlapping results — e.g. //dt/following-sibling::dd from
        # 50 sibling dts each emits a long suffix of overlapping dds.
        # Deduplicate by node identity so callers see set semantics.
        dedupe_node_set(out)
      end

      def dedupe_node_set(nodes)
        return nodes if nodes.length < 2
        seen = {}
        out = []
        nodes.each do |n|
          key = node_identity(n)
          next if seen[key]
          seen[key] = true
          out << n
        end
        out
      end

      def apply_step_predicates(nodes, preds)
        preds.each do |pred_ast|
          filtered = []
          total = nodes.length
          nodes.each_with_index do |n, idx|
            ctx_info = { position: idx + 1, last: total }
            r = eval_expr(pred_ast, [n], ctx_info)
            keep =
              if r.is_a?(Numeric)
                # Numeric predicate: positional
                r.to_i == idx + 1
              else
                xpath_boolean(r)
              end
            filtered << n if keep
          end
          nodes = filtered
        end
        nodes
      end

      def apply_predicates(base, preds)
        nodes = base.is_a?(Array) ? base : [base]
        apply_step_predicates(nodes, preds)
      end

      # ---- Node identity / wrapping ------------------------------------

      def root_for_context
        @document.backing
      end

      def parent_of(n)
        return nil if n.nil?
        if native_node?(n)
          pid = n.doc.node_parent(n.id)
          pid ? wrap_native(pid) : @document
        elsif n.is_a?(Scrapetor::Native::DocumentWrapper) || n.is_a?(Scrapetor::Document)
          nil
        elsif n.is_a?(Scrapetor::Node)
          n.parent
        elsif n.respond_to?(:parent)
          n.parent
        end
      end

      def collect_self(n, nt, out)
        push_if_matches(n, nt, out)
      end

      def collect_children(n, nt, out)
        nd, rid, wrapper = arena_handle_for(n)
        if nd
          nd.node_children(rid).each do |cid|
            type = nd.node_type(cid)
            wrapped = wrap_native_typed_with(nd, cid, type, wrapper: wrapper)
            push_if_matches(wrapped, nt, out) if wrapped
          end
          return
        end
        if n.is_a?(Scrapetor::Document)
          n.backing.respond_to?(:children) ? n.backing.children.each { |c| push_if_matches(wrap_dom(c), nt, out) } : nil
        elsif n.is_a?(Scrapetor::Node)
          n.backing_node.children.each { |c| push_if_matches(wrap_dom(c), nt, out) }
        end
      end

      # Returns [native_doc, node_id, wrapper] when the node lives in
      # the arena, nil otherwise. Handles all three native carriers:
      # Scrapetor::Node wrapping a Native::Element, the
      # Native::DocumentWrapper itself (root context), and a raw
      # Native::Element.
      def arena_handle_for(n)
        if n.is_a?(Scrapetor::Node)
          bk = n.backing_node
          if bk.respond_to?(:id) && bk.respond_to?(:doc) && bk.doc.respond_to?(:node_following_sibling_ids)
            return [bk.doc, bk.id, (bk.respond_to?(:wrapper) ? bk.wrapper : nil)]
          end
        elsif n.is_a?(Scrapetor::Native::DocumentWrapper)
          return [n.native, 0, n]
        elsif n.respond_to?(:id) && n.respond_to?(:doc) && n.doc.respond_to?(:node_following_sibling_ids)
          # raw Native::Element
          return [n.doc, n.id, (n.respond_to?(:wrapper) ? n.wrapper : nil)]
        end
        nil
      end

      def collect_descendants(n, nt, out)
        nd, rid, wrapper = arena_handle_for(n)
        if nd
          # Comment-specific fast path: dedicated C primitive.
          if nt == :comment
            nd.node_descendant_comment_ids(rid).each { |cid|
              out << wrap_native_typed_with(nd, cid, 8, wrapper: wrapper)
            }
            return
          end
          collect_descendant_ids(nd, rid, nt, out, wrapper)
          return
        end
        if n.respond_to?(:children)
          stack = n.children.to_a.reverse
          while (c = stack.pop)
            push_if_matches(c.is_a?(Scrapetor::Node) ? c : wrap_dom(c), nt, out)
            if c.respond_to?(:children)
              kids = c.children.to_a
              stack.concat(kids.reverse)
            end
          end
        end
      end

      def collect_descendant_ids(nd, rid, nt, out, wrapper)
        # Range walk: ids (rid+1 .. dfs_out(rid)] are descendants. Filter
        # by node test and push wrapped results. Skips non-elements
        # unless the test wants them.
        # For DocumentWrapper rid=0, we want to enumerate all descendants
        # which means everything in the arena from id 1 up.
        size = nd.size
        lo = rid + 1
        hi = size - 1
        # We can fall back to a generic stack walk if needed, but ids
        # are pre-order in the unmutated case, so the range walk is exact.
        # node_type call avoids loading nodes we'll skip immediately.
        case nt
        when :any_element, :node
          (lo..hi).each do |k|
            t = nd.node_type(k)
            next unless t == 1 || (nt == :node && (t == 1 || t == 3 || t == 8))
            out << wrap_native_typed_with(nd, k, t, wrapper: wrapper)
          end
        when :text
          (lo..hi).each do |k|
            t = nd.node_type(k)
            next unless t == 3
            out << wrap_native_typed_with(nd, k, t, wrapper: wrapper)
          end
        when :comment
          (lo..hi).each do |k|
            t = nd.node_type(k)
            next unless t == 8
            out << wrap_native_typed_with(nd, k, t, wrapper: wrapper)
          end
        when Hash
          if (name = nt[:name])
            target = name.downcase
            (lo..hi).each do |k|
              next unless nd.node_type(k) == 1
              n = nd.node_name(k)
              next unless n.casecmp(target).zero?
              out << wrap_native_typed_with(nd, k, 1, wrapper: wrapper)
            end
          end
        end
      end

      def ancestors_of(n)
        nd, rid, wrapper = arena_handle_for(n)
        if nd
          nd.node_ancestor_ids(rid).map { |i| wrap_native_typed_with(nd, i, 1, wrapper: wrapper) }
        elsif n.is_a?(Scrapetor::Node)
          list = []
          cur = n.parent
          while cur
            list << cur
            cur = cur.parent
          end
          list.reverse
        else
          []
        end
      end

      def following_siblings_of(n)
        nd, rid, wrapper = arena_handle_for(n)
        if nd
          nd.node_following_sibling_ids(rid).map { |i| wrap_native_typed_with(nd, i, 1, wrapper: wrapper) }
        elsif n.is_a?(Scrapetor::Node)
          out = []
          cur = n.next_sibling
          while cur
            out << cur if cur.respond_to?(:element?) && cur.element?
            cur = cur.respond_to?(:next_sibling) ? cur.next_sibling : nil
          end
          out
        else
          []
        end
      end

      def preceding_siblings_of(n)
        nd, rid, wrapper = arena_handle_for(n)
        if nd
          nd.node_preceding_sibling_ids(rid).map { |i| wrap_native_typed_with(nd, i, 1, wrapper: wrapper) }
        elsif n.is_a?(Scrapetor::Node)
          out = []
          cur = n.previous_sibling
          while cur
            out.unshift(cur) if cur.respond_to?(:element?) && cur.element?
            cur = cur.respond_to?(:previous_sibling) ? cur.previous_sibling : nil
          end
          out
        else
          []
        end
      end

      def following_of(n)
        nd, rid, wrapper = arena_handle_for(n)
        return [] unless nd
        nd.node_following_ids(rid).map { |i| wrap_native_typed_with(nd, i, 1, wrapper: wrapper) }
      end

      def preceding_of(n)
        nd, rid, wrapper = arena_handle_for(n)
        return [] unless nd
        nd.node_preceding_ids(rid).map { |i| wrap_native_typed_with(nd, i, 1, wrapper: wrapper) }
      end

      def collect_attributes(n, nt, out)
        if native_node?(n)
          attrs = n.doc.node_attributes(n.id)
          attrs.each do |name, val|
            case nt
            when :any_element, :any_node, :node
              out << val
            when Hash
              if nt[:name].nil? || nt[:name] == "*" || name.casecmp(nt[:name]).zero?
                out << val
              end
            end
          end
        elsif n.is_a?(Scrapetor::Node)
          attrs = n.attributes
          attrs.each do |name, val|
            case nt
            when Hash
              if nt[:name].nil? || nt[:name] == "*" || name.casecmp(nt[:name]).zero?
                out << val
              end
            else
              out << val
            end
          end
        end
      end

      def push_if_matches(n, nt, out)
        return unless matches_node_test?(n, nt)
        out << n
      end

      def matches_node_test?(n, nt)
        case nt
        when :any_element
          n.respond_to?(:element?) ? n.element? : (n.respond_to?(:name) && !n.name.start_with?("#"))
        when :text
          n.respond_to?(:text?) && n.text?
        when :comment
          n.respond_to?(:comment?) && n.comment?
        when :node
          true
        when Hash
          return false unless n.respond_to?(:name)
          target = nt[:name]
          return false if target.nil?
          return true if target == "*"
          name = n.name
          return false if name.nil? || name.start_with?("#")
          name.casecmp(target).zero?
        else
          false
        end
      end

      # ---- Native wrapping helpers --------------------------------------

      def native_handles_for(context)
        if context.is_a?(Scrapetor::Document)
          bk = context.backing
          if defined?(Scrapetor::Native::DocumentWrapper) && bk.is_a?(Scrapetor::Native::DocumentWrapper) &&
             bk.native.respond_to?(:node_following_sibling_ids)
            return [bk.native, 0]
          end
        elsif context.is_a?(Scrapetor::Node)
          bk = context.backing_node
          if bk.respond_to?(:id) && bk.respond_to?(:doc) && bk.doc.respond_to?(:node_following_sibling_ids)
            return [bk.doc, bk.id]
          end
        end
        [nil, nil]
      end

      def native_node?(n)
        return false unless n
        n.respond_to?(:id) && n.respond_to?(:doc) && n.doc.respond_to?(:node_following_sibling_ids)
      end

      def native_wrapper_for(n)
        return n if n.is_a?(Scrapetor::Native::DocumentWrapper)
        return n.wrapper if n.respond_to?(:wrapper)
        nil
      end

      def wrap_native(id)
        return nil if @native_doc.nil? || id.nil?
        wrap_native_typed(id, @native_doc.node_type(id))
      end

      def wrap_native_typed(id, type)
        wrap_native_typed_with(@native_doc, id, type, wrapper: @initial.respond_to?(:wrapper) ? @initial.wrapper : nil)
      end

      def wrap_native_typed_with(nd, id, type, wrapper: nil)
        case type
        when 1
          Scrapetor::Node.new(@document, Scrapetor::Native::Element.new(nd, id, wrapper))
        when 8
          Scrapetor::CommentNode.new(@document, nd.node_comment_text(id))
        when 3
          # text node — use TextNode (String subclass that responds to
          # text?, name, etc.) so XPath predicates against text-node
          # sets behave like Nokogiri's.
          Scrapetor::TextNode.new(nd.node_text(id))
        else
          # doc / unknown
          nil
        end
      end

      def wrap_dom(node)
        return node if node.is_a?(Scrapetor::Node) || node.is_a?(Scrapetor::CommentNode)
        if node.respond_to?(:comment?) && node.comment?
          Scrapetor::CommentNode.new(@document, node.respond_to?(:content) ? node.content : node.to_s)
        elsif node.respond_to?(:text?) && node.text?
          node.respond_to?(:content) ? node.content : node.to_s
        elsif node.respond_to?(:element?) && node.element?
          Scrapetor::Node.new(@document, node)
        else
          node
        end
      end

      def node_identity(n)
        if n.is_a?(Scrapetor::Node)
          bk = n.backing_node
          bk.respond_to?(:id) ? [:nat, bk.respond_to?(:doc) ? bk.doc.object_id : nil, bk.id] : bk.object_id
        else
          n.object_id
        end
      end

      # ---- XPath type coercions ----------------------------------------

      def xpath_boolean(v)
        case v
        when nil          then false
        when true, false  then v
        when Numeric      then !(v.zero? || (v.respond_to?(:nan?) && v.nan?))
        when String       then !v.empty?
        when Array        then !v.empty?
        else true
        end
      end

      def xpath_string(v)
        case v
        when nil            then ""
        when String         then v
        when true           then "true"
        when false          then "false"
        when Float
          if v.nan?            then "NaN"
          elsif v.infinite?    then v.positive? ? "Infinity" : "-Infinity"
          elsif v == v.to_i    then v.to_i.to_s
          else v.to_s
          end
        when Numeric        then v.to_s
        when Array
          n = v.first
          xpath_string_for_node(n)
        else
          xpath_string_for_node(v)
        end
      end

      def xpath_string_for_node(n)
        return "" if n.nil?
        return n if n.is_a?(String)
        if n.respond_to?(:text)
          n.text.to_s
        elsif n.respond_to?(:to_s)
          n.to_s
        else
          ""
        end
      end

      def xpath_number(v)
        case v
        when nil      then Float::NAN
        when Numeric  then v
        when true     then 1
        when false    then 0
        when String
          s = v.strip
          return Float::NAN if s.empty?
          if s =~ /\A-?\d+\.?\d*\z/ || s =~ /\A-?\.\d+\z/
            s.include?(".") ? s.to_f : s.to_i
          else
            Float::NAN
          end
        when Array
          xpath_number(xpath_string(v))
        else
          xpath_number(xpath_string(v))
        end
      end

      # ---- Comparison rules (XPath 1.0 §3.4) --------------------------

      def do_compare(op, l, r)
        # If either operand is a node-set, the comparison is true if any
        # node satisfies the condition against the other operand.
        if l.is_a?(Array) || r.is_a?(Array)
          a = l.is_a?(Array) ? l : [l]
          b = r.is_a?(Array) ? r : [r]
          return compare_node_sets(op, a, b)
        end
        case op
        when :eq, :neq
          # If neither is a node-set, type coercion:
          # - if either is boolean → both booleans
          # - else if either is number → both numbers
          # - else → both strings
          if l.is_a?(TrueClass) || l.is_a?(FalseClass) ||
             r.is_a?(TrueClass) || r.is_a?(FalseClass)
            res = xpath_boolean(l) == xpath_boolean(r)
          elsif l.is_a?(Numeric) || r.is_a?(Numeric)
            res = xpath_number(l) == xpath_number(r)
          else
            res = xpath_string(l) == xpath_string(r)
          end
          op == :eq ? res : !res
        else
          ln = xpath_number(l); rn = xpath_number(r)
          return false if (ln.is_a?(Float) && ln.nan?) || (rn.is_a?(Float) && rn.nan?)
          case op
          when :lt then ln <  rn
          when :le then ln <= rn
          when :gt then ln >  rn
          when :ge then ln >= rn
          end
        end
      end

      def compare_node_sets(op, a, b)
        case op
        when :eq, :neq
          # Stringify each side; check if any pair matches under XPath rules.
          a.each do |x|
            sx = xpath_string(x)
            b.each do |y|
              sy = xpath_string(y)
              hit = sx == sy
              return op == :eq if hit && op == :eq
              return op == :neq if !hit && op == :neq
            end
          end
          op == :neq && a.empty? && b.empty? ? false : (op == :neq ? a.any? { |x| b.any? { |y| xpath_string(x) != xpath_string(y) } } : false)
        else
          # Numeric: any pair satisfies the comparison.
          a.each do |x|
            nx = xpath_number(x)
            next if nx.is_a?(Float) && nx.nan?
            b.each do |y|
              ny = xpath_number(y)
              next if ny.is_a?(Float) && ny.nan?
              ok =
                case op
                when :lt then nx <  ny
                when :le then nx <= ny
                when :gt then nx >  ny
                when :ge then nx >= ny
                end
              return true if ok
            end
          end
          false
        end
      end

      # ---- Functions ----------------------------------------------------

      def call_function(name, args, context_set, position_info)
        case name
        # node-set
        when "last"
          (position_info && position_info[:last]) || context_set.length
        when "position"
          (position_info && position_info[:position]) || 1
        when "count"
          v = eval_expr(args[0], context_set, position_info)
          v.is_a?(Array) ? v.length : 0
        when "id"
          # id('foo') — return element with that id from the document.
          v = eval_expr(args[0], context_set, position_info)
          ids = v.is_a?(Array) ? v.map { |x| xpath_string(x) }.flat_map { |s| s.split(/\s+/) } : xpath_string(v).split(/\s+/)
          out = []
          ids.each do |id_str|
            hit = @document.at_css("##{id_str}") rescue nil
            out << hit if hit
          end
          out
        when "local-name"
          n = arg_first_node(args, context_set, position_info)
          n && n.respond_to?(:name) ? n.name.split(":").last.to_s : ""
        when "name"
          n = arg_first_node(args, context_set, position_info)
          n && n.respond_to?(:name) ? n.name.to_s : ""
        when "namespace-uri"
          ""  # we don't model namespaces in HTML
        # string
        when "string"
          xpath_string(args.empty? ? context_set.first : eval_expr(args[0], context_set, position_info))
        when "concat"
          args.map { |a| xpath_string(eval_expr(a, context_set, position_info)) }.join
        when "starts-with"
          xpath_string(eval_expr(args[0], context_set, position_info))
            .start_with?(xpath_string(eval_expr(args[1], context_set, position_info)))
        when "contains"
          xpath_string(eval_expr(args[0], context_set, position_info))
            .include?(xpath_string(eval_expr(args[1], context_set, position_info)))
        when "substring-before"
          a = xpath_string(eval_expr(args[0], context_set, position_info))
          b = xpath_string(eval_expr(args[1], context_set, position_info))
          idx = a.index(b)
          idx ? a[0...idx] : ""
        when "substring-after"
          a = xpath_string(eval_expr(args[0], context_set, position_info))
          b = xpath_string(eval_expr(args[1], context_set, position_info))
          idx = a.index(b)
          idx ? a[(idx + b.length)..] || "" : ""
        when "substring"
          s = xpath_string(eval_expr(args[0], context_set, position_info))
          start = xpath_number(eval_expr(args[1], context_set, position_info))
          # XPath substring is 1-based, rounding to nearest integer.
          start_i = start.respond_to?(:round) ? start.round.to_i : start.to_i
          if args.size > 2
            len = xpath_number(eval_expr(args[2], context_set, position_info))
            len_i = len.respond_to?(:round) ? len.round.to_i : len.to_i
            from = [start_i, 1].max
            to   = start_i + len_i
            from_i = from - 1
            to_i   = [to - 1, s.length].min
            s[from_i...to_i] || ""
          else
            from = [start_i, 1].max
            s[(from - 1)..] || ""
          end
        when "string-length"
          s = args.empty? ?
                xpath_string(context_set.first) :
                xpath_string(eval_expr(args[0], context_set, position_info))
          s.length
        when "normalize-space"
          s = args.empty? ?
                xpath_string(context_set.first) :
                xpath_string(eval_expr(args[0], context_set, position_info))
          s.strip.gsub(/\s+/, " ")
        when "translate"
          s    = xpath_string(eval_expr(args[0], context_set, position_info))
          from = xpath_string(eval_expr(args[1], context_set, position_info))
          to   = xpath_string(eval_expr(args[2], context_set, position_info))
          # Per XPath: characters in `from` are replaced by the same-index
          # char in `to`; characters in `from` past `to`'s length are deleted.
          map = {}
          from.each_char.with_index { |c, i| map[c] = i < to.length ? to[i] : nil }
          s.chars.map { |c| map.key?(c) ? map[c] : c }.compact.join
        # boolean
        when "boolean" then xpath_boolean(eval_expr(args[0], context_set, position_info))
        when "not"     then !xpath_boolean(eval_expr(args[0], context_set, position_info))
        when "true"    then true
        when "false"   then false
        when "lang"
          # lang('en') — true if context node's xml:lang ancestor-or-self
          # starts with 'en' (case-insensitive). HTML: also `lang` attr.
          target = xpath_string(eval_expr(args[0], context_set, position_info)).downcase
          n = context_set.first
          n = n.is_a?(Array) ? n.first : n
          while n
            lang = nil
            if n.respond_to?(:[])
              lang = (n["xml:lang"] || n["lang"]) rescue nil
            end
            return true if lang && (lang.downcase == target || lang.downcase.start_with?("#{target}-"))
            n = parent_of(n)
          end
          false
        # number
        when "number"
          xpath_number(args.empty? ? context_set.first : eval_expr(args[0], context_set, position_info))
        when "sum"
          v = eval_expr(args[0], context_set, position_info)
          v = [v] unless v.is_a?(Array)
          v.inject(0.0) { |acc, x| acc + xpath_number(x).to_f }
        when "floor"
          xpath_number(eval_expr(args[0], context_set, position_info)).floor
        when "ceiling"
          xpath_number(eval_expr(args[0], context_set, position_info)).ceil
        when "round"
          n = xpath_number(eval_expr(args[0], context_set, position_info))
          n.is_a?(Float) && n.nan? ? n : n.round
        else
          raise UnsupportedError, "unknown XPath function `#{name}()`"
        end
      end

      def arg_first_node(args, context_set, position_info)
        v = args.empty? ? context_set : eval_expr(args[0], context_set, position_info)
        v.is_a?(Array) ? v.first : v
      end
    end
  end
end
