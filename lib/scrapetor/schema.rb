# frozen_string_literal: true

module Scrapetor
  class Schema
    Field = Struct.new(
      :name, :selector, :attr, :attr_str, :type, :clean, :multi,
      :normalize_url, :default, :required, :transform, :delimiter
    )
    Group = Struct.new(:name, :selector, :fields, :groups)

    attr_reader :fields, :groups

    def initialize
      @fields = []
      @groups = []
    end

    def self.build(&block)
      s = new
      s.instance_eval(&block) if block
      s
    end

    # field :name, from: SELECTOR, attr: SYM, type: SYM,
    #              clean: BOOL, multi: BOOL, normalize_url: BOOL,
    #              default: VALUE, required: BOOL,
    #              transform: PROC, delimiter: STRING_OR_REGEX
    #
    # from: may be a String selector or an Array of selectors (tried in
    # order until one matches).
    #
    # Types: :text :integer :float :money :url :date :json :html :list
    #        :boolean :array (alias for multi:true)
    def field(name,
              from:,
              attr: nil,
              type: :text,
              clean: false,
              multi: false,
              normalize_url: false,
              default: nil,
              required: false,
              transform: nil,
              delimiter: /\s*,\s*/)
      multi = true if type == :array
      type  = :text if type == :array
      @fields << Field.new(
        name, from, attr, attr && attr.to_s, type, clean, multi,
        normalize_url, default, required, transform, delimiter
      )
    end

    def repeated(selector, as:, &block)
      sub = self.class.build(&block)
      @groups << Group.new(as, selector, sub.fields, sub.groups)
    end

    # ----- Cross-process plan cache -----
    #
    # Serialize a schema to a binary blob (Marshal) so a worker can
    # restore the compiled descriptor without re-parsing the Ruby DSL.
    # Schemas using `transform:` (procs) can't be dumped — those plans
    # must be rebuilt from source.

    def dump
      Marshal.dump(self.class.dumpable(self))
    end

    def self.load(blob)
      new_from_h(Marshal.load(blob)) # rubocop:disable Security/MarshalLoad
    end

    def self.dump_to_file(schema, path)
      File.binwrite(path, schema.dump)
      path
    end

    def self.load_file(path)
      load(File.binread(path))
    end

    # Convert a schema to a portable Hash (no procs).
    def self.dumpable(schema)
      {
        fields: schema.fields.map { |f| field_to_h(f) },
        groups: schema.groups.map { |g| group_to_h(g) }
      }
    end

    def self.field_to_h(f)
      raise SchemaError, "transform: blocks can't be serialized" if f.transform
      {
        name:          f.name,
        selector:      f.selector,
        attr:          f.attr,
        attr_str:      f.attr_str,
        type:          f.type,
        clean:         f.clean,
        multi:         f.multi,
        normalize_url: f.normalize_url,
        default:       f.default,
        required:      f.required,
        delimiter:     f.delimiter
      }
    end

    def self.group_to_h(g)
      {
        name:     g.name,
        selector: g.selector,
        fields:   g.fields.map { |f| field_to_h(f) },
        groups:   g.groups.map { |sub| group_to_h(sub) }
      }
    end

    def self.new_from_h(h)
      schema = new
      h[:fields].each { |fh| schema.fields << field_from_h(fh) }
      h[:groups].each { |gh| schema.groups << group_from_h(gh) }
      schema
    end

    def self.field_from_h(h)
      Field.new(
        h[:name], h[:selector], h[:attr], h[:attr_str], h[:type],
        h[:clean], h[:multi], h[:normalize_url], h[:default],
        h[:required], nil, h[:delimiter]
      )
    end

    def self.group_from_h(h)
      Group.new(
        h[:name],
        h[:selector],
        h[:fields].map { |fh| field_from_h(fh) },
        h[:groups].map { |gh| group_from_h(gh) }
      )
    end

    def to_h
      self.class.dumpable(self)
    end
  end
end
