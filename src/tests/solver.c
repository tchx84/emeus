#include "emeus-expression-private.h"
#include "emeus-simplex-solver-private.h"
#include "emeus-types-private.h"
#include "emeus-variable-private.h"

#include "emeus-test-utils.h"

static void
emeus_solver_simple (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 167.0);
  Variable *y = simplex_solver_create_variable (&solver, "y", 2.0);

  Expression *e = expression_new_from_variable (y);

  simplex_solver_add_constraint (&solver,
                                 x, OPERATOR_TYPE_EQ, e,
                                 STRENGTH_REQUIRED);

  double x_value = variable_get_value (x);
  double y_value = variable_get_value (y);

  emeus_assert_almost_equals (x_value, y_value);
  emeus_assert_almost_equals (x_value, 0.0);
  emeus_assert_almost_equals (y_value, 0.0);

  expression_unref (e);
  variable_unref (y);
  variable_unref (x);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_stay (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 5.0);
  Variable *y = simplex_solver_create_variable (&solver, "y", 10.0);

  simplex_solver_add_stay_variable (&solver, x, STRENGTH_WEAK);
  simplex_solver_add_stay_variable (&solver, y, STRENGTH_WEAK);

  double x_value = variable_get_value (x);
  double y_value = variable_get_value (y);

  emeus_assert_almost_equals (x_value,  5.0);
  emeus_assert_almost_equals (y_value, 10.0);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_variable_geq_constant (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 10.0);
  Expression *e = simplex_solver_create_expression (&solver, 100.0);

  simplex_solver_add_constraint (&solver, x, OPERATOR_TYPE_GE, e, STRENGTH_REQUIRED);

  double x_value = variable_get_value (x);

  emeus_assert_almost_equals (x_value, 100.0);

  expression_unref (e);
  variable_unref (x);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_variable_leq_constant (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 100.0);
  Expression *e = simplex_solver_create_expression (&solver, 10.0);

  simplex_solver_add_constraint (&solver, x, OPERATOR_TYPE_LE, e, STRENGTH_REQUIRED);

  double x_value = variable_get_value (x);

  emeus_assert_almost_equals (x_value, 10.0);

  expression_unref (e);
  variable_unref (x);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_variable_eq_constant (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 10.0);
  Expression *e = simplex_solver_create_expression (&solver, 100.0);

  simplex_solver_add_constraint (&solver, x, OPERATOR_TYPE_EQ, e, STRENGTH_REQUIRED);

  double x_value = variable_get_value (x);

  emeus_assert_almost_equals (x_value, 100.0);

  expression_unref (e);
  variable_unref (x);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_eq_with_stay (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 10.0);
  Variable *width = simplex_solver_create_variable (&solver, "width", 10.0);
  Variable *right_min = simplex_solver_create_variable (&solver, "rightMin", 100.0);
  Expression *right = expression_plus_variable (expression_new_from_variable (x), width);

  simplex_solver_add_stay_variable (&solver, width, STRENGTH_WEAK);
  simplex_solver_add_stay_variable (&solver, right_min, STRENGTH_WEAK);
  simplex_solver_add_constraint (&solver, right_min, OPERATOR_TYPE_EQ, right, STRENGTH_REQUIRED);

  double x_value = variable_get_value (x);
  double width_value = variable_get_value (width);

  emeus_assert_almost_equals (x_value, 90.0);
  emeus_assert_almost_equals (width_value, 10.0);

  expression_unref (right);
  variable_unref (right_min);
  variable_unref (width);
  variable_unref (x);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_cassowary (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *x = simplex_solver_create_variable (&solver, "x", 0.0);
  Variable *y = simplex_solver_create_variable (&solver, "y", 0.0);

  simplex_solver_add_constraint (&solver,
                                 x, OPERATOR_TYPE_LE, expression_new_from_variable (y),
                                 STRENGTH_REQUIRED);
  simplex_solver_add_constraint (&solver,
                                 y, OPERATOR_TYPE_EQ, expression_plus (expression_new_from_variable (x), 3.0),
                                 STRENGTH_REQUIRED);
  simplex_solver_add_constraint (&solver,
                                 x, OPERATOR_TYPE_EQ, expression_new_from_constant (10.0),
                                 STRENGTH_WEAK);
  simplex_solver_add_constraint (&solver,
                                 y, OPERATOR_TYPE_EQ, expression_new_from_constant (10.0),
                                 STRENGTH_WEAK);

  double x_val = variable_get_value (x);
  double y_val = variable_get_value (y);

  if (g_test_verbose ())
    g_print ("x = %g, y = %g\n", x_val, y_val);

  g_assert_true ((emeus_fuzzy_equals (x_val, 10, 1e-8) && emeus_fuzzy_equals (y_val, 13, 1e-8)) ||
                 (emeus_fuzzy_equals (x_val,  7, 1e-8) && emeus_fuzzy_equals (y_val, 10, 1e-9)));

  simplex_solver_clear (&solver);
}

static void
emeus_solver_edit_var_required (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *a = simplex_solver_create_variable (&solver, "a", 0.0);
  simplex_solver_add_stay_variable (&solver, a, STRENGTH_STRONG);

  emeus_assert_almost_equals (variable_get_value (a), 0.0);

  simplex_solver_add_edit_variable (&solver, a, STRENGTH_REQUIRED);
  simplex_solver_begin_edit (&solver);
  simplex_solver_suggest_value (&solver, a, 2.0);
  simplex_solver_end_edit (&solver);

  emeus_assert_almost_equals (variable_get_value (a), 2.0);

  simplex_solver_clear (&solver);
}

static void
emeus_solver_edit_var_suggest (void)
{
  SimplexSolver solver = SIMPLEX_SOLVER_INIT;

  simplex_solver_init (&solver);

  Variable *a = simplex_solver_create_variable (&solver, "a", 0.0);
  Variable *b = simplex_solver_create_variable (&solver, "b", 0.0);

  simplex_solver_add_stay_variable (&solver, a, STRENGTH_STRONG);
  simplex_solver_add_constraint (&solver, a, OPERATOR_TYPE_EQ, expression_new_from_variable (b), STRENGTH_REQUIRED);
  simplex_solver_resolve (&solver);

  emeus_assert_almost_equals (variable_get_value (a), 0.0);
  emeus_assert_almost_equals (variable_get_value (b), 0.0);

  simplex_solver_add_edit_variable (&solver, a, STRENGTH_REQUIRED);
  simplex_solver_begin_edit (&solver);
  simplex_solver_suggest_value (&solver, a, 2.0);
  simplex_solver_resolve (&solver);

  emeus_assert_almost_equals (variable_get_value (a), 2.0);
  emeus_assert_almost_equals (variable_get_value (b), 2.0);

  simplex_solver_suggest_value (&solver, a, 10.0);
  simplex_solver_resolve (&solver);

  emeus_assert_almost_equals (variable_get_value (a), 10.0);
  emeus_assert_almost_equals (variable_get_value (b), 10.0);

  simplex_solver_clear (&solver);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/emeus/solver/simple", emeus_solver_simple);
  g_test_add_func ("/emeus/solver/stay", emeus_solver_stay);
  g_test_add_func ("/emeus/solver/edit-var-required", emeus_solver_edit_var_required);
  g_test_add_func ("/emeus/solver/edit-var-suggest", emeus_solver_edit_var_suggest);
  g_test_add_func ("/emeus/solver/variable-geq-constant", emeus_solver_variable_geq_constant);
  g_test_add_func ("/emeus/solver/variable-leq-constant", emeus_solver_variable_leq_constant);
  g_test_add_func ("/emeus/solver/variable-eq-constant", emeus_solver_variable_eq_constant);
  g_test_add_func ("/emeus/solver/eq-with-stay", emeus_solver_eq_with_stay);
  g_test_add_func ("/emeus/solver/cassowary", emeus_solver_cassowary);

  return g_test_run ();
}
