# frozen_string_literal: true

module Scrapetor
  # Phase 1: in-process registry mapping structural fingerprints to
  # compiled extraction plans (Schema instances). Phase 8 (per plan.md)
  # promotes this to an mmap-backed cross-process store.
  class TemplateRegistry
    def initialize
      @plans = {}
    end

    def store(fingerprint, plan)
      @plans[fingerprint] = plan
    end

    def fetch(fingerprint)
      @plans[fingerprint]
    end

    def size
      @plans.size
    end
  end
end
