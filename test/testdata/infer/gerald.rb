# typed: strict
require_relative '../../../extn'
Opus::AutogenLoader.init(__FILE__) # error: Unable to resolve constant

module Opus::CIBot::Gerald

  class MatchTimeout < StandardError

    attr_reader :rule_token # error: Use of undeclared variable

    def initialize(message, rule_token: '')
      super(message)
      @rule_token = rule_token
    end
  end

  class Matcher
    include Chalk::Log # error: Unable to resolve constant

    MAX_AFFECTED_FILES = 100

    def initialize
      @rules, invalid_rules =  # error: undeclared variable
              Opus::CIBot::Model::GeraldRule.query_by(:deleted_at_is_nil).load_all({}).  # error: Unable to resolve constant
                partition(&:valid?)
      if !invalid_rules.empty?
        invalid_rule_ids = invalid_rules.map(&:token).join(',')
        log.warn('Gerald skipping invalid rules: ' + invalid_rule_ids) # error: Method `log` does not exist on `Opus::CIBot::Gerald::Matcher`
      end
    end

    # @raise [MatchTimeout]
    def match(match_context)
      # Sadly we process diff files not patch files so we don't know the actual
      # number of commits. I don't think it is worth switching right now, but
      # maybe in the future.
      if match_context.diff.affected_files.count > MAX_AFFECTED_FILES
        log.warn("Gerald skipping large PR with #{match_context.diff.affected_files.count} affected files") # error: Method `log` does not exist on `Opus::CIBot::Gerald::Matcher`
        return []
      end

      budget = MatchTimeBudget.new
      @rules.select do |r|
        budget.time_rule(r) do
          r.matches?(match_context)
        end
      end
    end
  end

  class MatchContext
    attr_reader :repo           # error: Use of undeclared variable
    attr_reader :assignee       # error: Use of undeclared variable
    attr_reader :gh_user        # error: Use of undeclared variable
    attr_reader :merge_branch   # error: Use of undeclared variable
    attr_reader :body           # error: Use of undeclared variable
    attr_reader :title          # error: Use of undeclared variable
    attr_reader :diff           # error: Use of undeclared variable
    attr_reader :openapi_diff   # error: Use of undeclared variable

    def initialize(repo, assignee, gh_user, merge_branch, body, title, diff, openapi_diff)
      @repo = repo
      @assignee = assignee
      @gh_user = gh_user
      @merge_branch = merge_branch
      @body = body
      @title = title
      @diff = diff
      @openapi_diff = openapi_diff
    end

    # Should the name be suffixed with `-stripe`?
    def user_stripe_suffix?
      # non internal (GHE) repos need usernames to be suffixed with `-stripe`
      !@repo.start_with?('stripe-internal/')
    end
  end

  class MatchTimeBudget

    TOTAL_TIME_MS = 10000
    PER_RULE_MS = 2000

    def initialize
      @start =   # error: undeclared variable
        Time.now
    end

    def check!
      dur_ms = (Time.now - @start) * 1000
      if dur_ms > TOTAL_TIME_MS
        raise MatchTimeout.new("Gerald match time budged exceeded #{TOTAL_TIME_MS}ms")
      end
    end

    def time_rule(rule)
      rule_start = Time.now
      res = yield
      dur_ms = (Time.now - rule_start) * 1000
      if dur_ms > PER_RULE_MS
        raise MatchTimeout.new(
          "Gerald rule '#{rule.token}' exceeded per-rule time budget actual=#{dur_ms.to_i}ms budget=#{PER_RULE_MS}ms",
          rule_token: rule.token)
      end
      check! # Also check the total time

      res
    end
  end

  class Diff
    # Using `.diff` format from github
    def initialize(raw_diff)
      @raw = raw_diff # error: undeclared variable
      @parsed = parse(raw_diff) # error: undeclared variable
    end

    def affected_files
      added_files + deleted_files + changed_files
    end

    def added_files
      @parsed.select {|part| part[:a_name] == '/dev/null'}.map {|part| part[:b_name]}
    end

    def deleted_files
      @parsed.select {|part| part[:b_name] == '/dev/null'}.map {|part| part[:a_name]}
    end

    def changed_files
      @parsed.select {|part| part[:a_name] == part[:b_name]}.map {|part| part[:b_name]}
    end

    def added_lines
      @parsed.map {|part| part[:added_lines]}.flatten
    end

    def removed_lines
      @parsed.map {|part| part[:removed_lines]}.flatten
    end

    def changed_lines
      added_lines + removed_lines
    end

    def changed_openapi?
      changed_files.include?(Opus::CIBot::Actions::OpenAPI::SPEC_PATH) # error: Unable to resolve constant
    end

    private def parse(diff)
      parts = diff.split(/^diff [^\n]*\n/m)[
        1..-1]
      parts ||= []
      parts = T.cast(parts, T::Array[String])
      parts.map do |part|
        lines = part.split("\n")
        a_name = T.let(nil, T.nilable(String))
        b_name = T.let(nil, T.nilable(String))
        added_lines = T::Array[String].new
        removed_lines = T::Array[String].new
        lines.each do |line|
          if line.start_with?("index ", '@@', 'new file mode')
            next
          elsif line.start_with?('---')
            a_name = line[4..-1]
            a_name = a_name[2..-1] if a_name && a_name.start_with?('a/')
                                                                         # https://jira.corp.stripe.com/browse/RUBYPLAT-583
          elsif line.start_with?('+++')
            b_name = line[4..-1]
            b_name = b_name[2..-1] if b_name && b_name.start_with?('b/')
                                                                         # https://jira.corp.stripe.com/browse/RUBYPLAT-583
          elsif line.start_with?('+')
            added_lines << T.must(line[1..-1])
          elsif line.start_with?('-')
            removed_lines << T.must(line[1..-1])
          end
        end
        next if a_name.nil?
        {
          a_name: a_name,
          b_name: b_name,
          added_lines: added_lines,
          removed_lines: removed_lines,
        }
      end.compact
    end
  end
end
