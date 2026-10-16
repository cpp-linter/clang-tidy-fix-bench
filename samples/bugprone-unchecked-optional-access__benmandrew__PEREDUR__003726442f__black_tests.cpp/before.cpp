#include "runner/black.hpp"
#include "test_suite.hpp"
#include "test_support.hpp"

namespace {

void test_satisfiable_simple() {
    SatisfiabilityChecker checker;
    expect(checker.check_satisfiability("F p").value(),
           "black-runner: F p should be satisfiable");
}

void test_unsatisfiable_contradiction() {
    SatisfiabilityChecker checker;
    expect(!checker.check_satisfiability("p & !p").value(),
           "black-runner: p & !p should be unsatisfiable");
}

void test_satisfiable_ltl() {
    SatisfiabilityChecker checker;
    expect(checker.check_satisfiability("G F p").value(),
           "black-runner: G F p should be satisfiable");
}

void test_unsatisfiable_ltl() {
    SatisfiabilityChecker checker;
    expect(!checker.check_satisfiability("G !p & F p").value(),
           "black-runner: G !p & F p should be unsatisfiable");
}

}  // namespace

void run_black_runner_tests() {
    test_satisfiable_simple();
    test_unsatisfiable_contradiction();
    test_satisfiable_ltl();
    test_unsatisfiable_ltl();
}
