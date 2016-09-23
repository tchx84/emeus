/* emeus-simplex-solver.c: The constraint solver
 *
 * Copyright 2016  Endless
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "emeus-simplex-solver-private.h"

#include "emeus-types-private.h"
#include "emeus-variable-private.h"
#include "emeus-expression-private.h"
#include "emeus-utils-private.h"

#include <glib.h>
#include <string.h>
#include <math.h>
#include <float.h>

typedef struct {
  Constraint *constraint;

  Variable *eplus;
  Variable *eminus;

  double prev_edit_constraint;

  int index_;
} EditInfo;

static struct {
  Variable *eplus;
  Variable *eminus;
  double prev_constant;
} internal_expression = {
  NULL,
  NULL,
  0.0,
};

Constraint *
constraint_ref (Constraint *constraint)
{
  constraint->ref_count += 1;

  return constraint;
}

void
constraint_unref (Constraint *constraint)
{
  constraint->ref_count -= 1;

  if (constraint->ref_count == 0)
    {
      expression_unref (constraint->expression);
      g_slice_free (Constraint, constraint);
    }
}

void
simplex_solver_init (SimplexSolver *solver)
{
  memset (solver, 0, sizeof (SimplexSolver));

  /* HashTable<Variable, HashSet<Variable>>; owns keys and values */
  solver->columns = g_hash_table_new_full (NULL, NULL,
                                           (GDestroyNotify) variable_unref,
                                           (GDestroyNotify) g_hash_table_unref);

  /* HashTable<Variable, Expression>; owns keys and values */
  solver->rows = g_hash_table_new_full (NULL, NULL,
                                        (GDestroyNotify) variable_unref,
                                        (GDestroyNotify) expression_unref);

  /* HashTable<Variable, Expression>; does not own keys or values */
  solver->external_rows = g_hash_table_new (NULL, NULL);

  /* HashSet<Variable>; does not own keys */
  solver->infeasible_rows = g_hash_table_new (NULL, NULL);

  /* HashSet<Variable>; does not own keys */
  solver->updated_externals = g_hash_table_new (NULL, NULL);

  solver->stay_plus_error_vars = g_ptr_array_new ();
  solver->stay_minus_error_vars = g_ptr_array_new ();

  solver->marker_vars = g_hash_table_new_full (NULL, NULL,
                                               NULL,
                                               (GDestroyNotify) variable_unref);

  /* The rows table owns the objective variable */
  solver->objective = variable_new (solver, VARIABLE_OBJECTIVE);
  g_hash_table_insert (solver->rows, solver->objective, expression_new (solver, 0.0));

  solver->slack_counter = 0;
  solver->dummy_counter = 0;
  solver->artificial_counter = 0;

  solver->needs_solving = false;
  solver->auto_solve = false;
}

void
simplex_solver_clear (SimplexSolver *solver)
{
  g_clear_pointer (&solver->columns, g_hash_table_unref);
  g_clear_pointer (&solver->rows, g_hash_table_unref);
  g_clear_pointer (&solver->external_rows, g_hash_table_unref);
  g_clear_pointer (&solver->infeasible_rows, g_hash_table_unref);
  g_clear_pointer (&solver->updated_externals, g_hash_table_unref);
  g_clear_pointer (&solver->marker_vars, g_hash_table_unref);

  g_clear_pointer (&solver->stay_plus_error_vars, g_ptr_array_unref);
  g_clear_pointer (&solver->stay_minus_error_vars, g_ptr_array_unref);
}

static GHashTable *
simplex_solver_get_column_set (SimplexSolver *solver,
                               Variable *param_var)
{
  return g_hash_table_lookup (solver->columns, param_var);
}

static bool
simplex_solver_column_has_key (SimplexSolver *solver,
                               Variable *subject)
{
  return g_hash_table_lookup (solver->columns, subject) != NULL;
}

static void
simplex_solver_insert_column_variable (SimplexSolver *solver,
                                       Variable *param_var,
                                       Variable *row_var)
{
  GHashTable *row_set;

  row_set = simplex_solver_get_column_set (solver, param_var);
  if (row_set == NULL)
    {
      row_set = g_hash_table_new_full (NULL, NULL,
                                       (GDestroyNotify) variable_unref,
                                       NULL);
      g_hash_table_insert (solver->columns, variable_ref (param_var), row_set);
    }

  if (row_var != NULL)
    g_hash_table_add (row_set, variable_ref (row_var));
}

static void
simplex_solver_insert_error_variable (SimplexSolver *solver,
                                      Constraint *constraint,
                                      Variable *variable)
{
  GHashTable *constraint_set;

  constraint_set = g_hash_table_lookup (solver->error_vars, constraint);
  if (constraint_set == NULL)
    {
      constraint_set = g_hash_table_new (NULL, NULL);
      g_hash_table_insert (solver->error_vars, constraint, constraint_set);
    }

  g_hash_table_add (constraint_set, variable);
}

static void
simplex_solver_reset_stay_constraints (SimplexSolver *solver)
{
  int i;

  for (i = 0; i < solver->stay_plus_error_vars->len; i++)
    {
      Variable *variable = g_ptr_array_index (solver->stay_plus_error_vars, i);
      Expression *expression;

      expression = g_hash_table_lookup (solver->rows, variable);
      if (expression == NULL)
        {
          variable = g_ptr_array_index (solver->stay_minus_error_vars, i);
          expression = g_hash_table_lookup (solver->rows, variable);
        }

      if (expression != NULL)
        expression_set_constant (expression, 0.0);
    }
}

static void
simplex_solver_set_external_variables (SimplexSolver *solver)
{
  GHashTableIter iter;
  gpointer key_p;

  g_hash_table_iter_init (&iter, solver->updated_externals);
  while (g_hash_table_iter_next (&iter, &key_p, NULL))
    {
      Variable *variable = key_p;
      Expression *expression;

      expression = g_hash_table_lookup (solver->external_rows, variable);
      if (expression == NULL)
        {
          variable_set_value (variable, 0.0);
          return;
        }

      variable_set_value (variable, expression_get_constant (expression));
    }

  g_hash_table_remove_all (solver->updated_externals);
  solver->needs_solving = false;
}

typedef struct {
  Variable *variable;
  SimplexSolver *solver;
} ForeachClosure;

static void
insert_expression_columns (Term *term,
                           gpointer data_)
{
  ForeachClosure *data = data_;

  simplex_solver_insert_column_variable (data->solver,
                                         term_get_variable (term),
                                         data->variable);
}

static void
simplex_solver_add_row (SimplexSolver *solver,
                        Variable *variable,
                        Expression *expression)
{
  ForeachClosure data;

  g_hash_table_insert (solver->rows, variable_ref (variable), expression_ref (expression));

  data.variable = variable;
  data.solver = solver;
  expression_terms_foreach (expression,
                            insert_expression_columns,
                            &data);

  if (variable_is_external (variable))
    g_hash_table_insert (solver->external_rows, variable, expression);
}

static void
simplex_solver_remove_column (SimplexSolver *solver,
                              Variable *variable)
{
  GHashTable *row_set = g_hash_table_lookup (solver->columns, variable);
  GHashTableIter iter;
  gpointer key_p;

  if (row_set == NULL)
    goto out;

  g_hash_table_iter_init (&iter, row_set);
  while (g_hash_table_iter_next (&iter, &key_p, NULL))
    {
      Expression *e = key_p;

      expression_remove_variable (e, variable);
    }

  g_hash_table_remove (row_set, variable);

out:
  if (variable_is_external (variable))
    g_hash_table_remove (solver->external_rows, variable);
}

static void
remove_expression_columns (Term *term,
                           gpointer data_)
{
  ForeachClosure *data = data_;

  GHashTable *row_set = g_hash_table_lookup (data->solver->columns, term_get_variable (term));

  if (row_set != NULL)
    g_hash_table_remove (row_set, data->variable);
}

static Expression *
simplex_solver_remove_row (SimplexSolver *solver,
                           Variable *variable)
{
  Expression *e = g_hash_table_lookup (solver->rows, variable);
  ForeachClosure data;

  g_assert (e != NULL);

  expression_ref (e);

  data.variable = variable;
  data.solver = solver;
  expression_terms_foreach (e,
                            remove_expression_columns,
                            &data);

  g_hash_table_remove (solver->infeasible_rows, variable);

  if (variable_is_external (variable))
    g_hash_table_remove (solver->external_rows, variable);

  g_hash_table_remove (solver->rows, variable);

  return e;
}

static void
expression_substitute_out (SimplexSolver *solver,
                           Expression *expression,
                           Variable *out_variable,
                           Expression *new_expression,
                           Variable *subject)
{
  double coefficient = expression_get_coefficient (expression, out_variable);

  expression_remove_variable (expression, out_variable);

  expression->constant += (coefficient * new_expression->constant);
}

static void
simplex_solver_substitute_out (SimplexSolver *solver,
                               Variable *old_variable,
                               Expression *expression)
{
  GHashTable *row_set = g_hash_table_lookup (solver->columns, old_variable);
  GHashTableIter iter;
  gpointer key_p;

  if (row_set == NULL)
    goto out;

  g_hash_table_iter_init (&iter, row_set);
  while (g_hash_table_iter_next (&iter, &key_p, NULL))
    {
      Variable *variable = key_p;
      Expression *e = g_hash_table_lookup (solver->rows, variable);

      expression_substitute_out (solver, e, old_variable, expression, variable);

      if (variable_is_external (variable))
        g_hash_table_add (solver->updated_externals, variable);

      if (variable_is_restricted (variable) && expression_get_constant (e) < 0)
        g_hash_table_add (solver->infeasible_rows, variable);
    }

out:
  if (variable_is_external (old_variable))
    g_hash_table_insert (solver->external_rows, variable_ref (old_variable), expression);

  g_hash_table_remove (solver->columns, old_variable);
}

static void
simplex_solver_pivot (SimplexSolver *solver,
                      Variable *entry_var,
                      Variable *exit_var)
{
  Expression *expr;

  expr = simplex_solver_remove_row (solver, exit_var);
  expression_change_subject (expr, exit_var, entry_var);

  simplex_solver_substitute_out (solver, entry_var, expr);
  simplex_solver_add_row (solver, entry_var, expr);
}

typedef struct {
  double objective_coefficient;
  Variable *entry_variable;
} NegativeClosure;

static void
find_negative_coefficient (Term *term,
                           gpointer data_)
{
  Variable *variable = term_get_variable (term);
  double coefficient = term_get_coefficient (term);
  NegativeClosure *data = data_;

  if (variable_is_pivotable (variable) && coefficient < data->objective_coefficient)
    {
      data->objective_coefficient = coefficient;
      data->entry_variable = variable;
    }
}

static void
simplex_solver_optimize (SimplexSolver *solver,
                         Variable *z)
{
  Variable *entry, *exit;
  Expression *z_row;

  solver->optimize_count += 1;

  z_row = g_hash_table_lookup (solver->rows, z);
  g_assert (z_row != NULL);

  entry = exit = NULL;

  while (true)
    {
      NegativeClosure data;
      GHashTable *column_vars;
      GHashTableIter iter;
      gpointer key_p;
      double min_ratio;
      double r;

      data.objective_coefficient = 0.0;
      data.entry_variable = NULL;

      expression_terms_foreach (z_row, find_negative_coefficient, &data);

      if (data.objective_coefficient >= -DBL_EPSILON)
        return;

      entry = data.entry_variable;

      min_ratio = DBL_MAX;
      r = 0;

      column_vars = simplex_solver_get_column_set (solver, entry);
      g_hash_table_iter_init (&iter, column_vars);
      while (g_hash_table_iter_next (&iter, &key_p, NULL))
        {
          Variable *v = key_p;
          Expression *expr;
          double coeff;

          if (!variable_is_pivotable (v))
            continue;

          expr = g_hash_table_lookup (solver->rows, v);
          coeff = expression_get_coefficient (expr, entry);
          if (coeff < 0.0)
            {
              r = -1.0 * expression_get_constant (expr) / coeff;
              if (r < min_ratio ||
                  (approx_val (r, min_ratio) && v < entry))
                {
                  min_ratio = r;
                  exit = v;
                }
            }
        }

      if (approx_val (min_ratio, DBL_MAX))
        g_critical ("Unbounded objective variable during optimization");
      else
        simplex_solver_pivot (solver, entry, exit);
    }
}

typedef struct {
  SimplexSolver *solver;
  Expression *expr;
} ReplaceClosure;

static void
replace_terms (Term *term,
               gpointer data_)
{
  Variable *v = term_get_variable (term);
  double c = term_get_coefficient (term);
  ReplaceClosure *data = data_;

  Expression *e = g_hash_table_lookup (data->solver->rows, v);

  if (e != NULL)
    expression_add_variable (data->expr, v, c);
  else
    expression_add_expression (data->expr, e, c, NULL);
}

static Expression *
simplex_solver_normalize_expression (SimplexSolver *solver,
                                     Constraint *constraint)
{
  Expression *cn_expr = constraint->expression;
  Expression *expr;
  Variable *slack_var, *dummy_var;
  Variable *eplus, *eminus;
  ReplaceClosure data;

  expr = expression_new (solver, expression_get_constant (cn_expr));

  data.solver = solver;
  data.expr = expr;
  expression_terms_foreach (cn_expr, replace_terms, &data);

  if (constraint_is_inequality (constraint))
    {
      /* If the constraint is an inequality, we add a slack variable to
       * turn it into an equality, e.g. from
       *
       *   expr >= 0
       *
       * to
       *
       *   expr - slack = 0
       *
       * Additionally, if the constraint is not required we add an
       * error variable:
       *
       *   expr - slack + error = 0
       */
      solver->slack_counter += 1;

      slack_var = variable_new (solver, VARIABLE_SLACK);
      expression_set_variable (expr, slack_var, -1.0);

      g_hash_table_insert (solver->marker_vars, constraint, slack_var);

      if (constraint->strength != STRENGTH_REQUIRED)
        {
          Expression *z_row;

          solver->slack_counter += 1;

          eminus = variable_new (solver, VARIABLE_SLACK);
          expression_set_variable (expr, eminus, 1.0);

          z_row = g_hash_table_lookup (solver->rows, solver->objective);
          expression_set_variable (z_row, eminus, constraint->strength);

          simplex_solver_insert_error_variable (solver, constraint, eminus);
          simplex_solver_add_variable (solver, eminus, solver->objective);
        }
    }
  else 
    {
      if (constraint_is_required (constraint))
        {
          /* If the constraint is required, we use a dummy marker variable;
           * the dummy won't be allowed to enter the basis of the tableau
           * when pivoting.
           */
          solver->dummy_counter += 1;

          dummy_var = variable_new (solver, VARIABLE_DUMMY);
          internal_expression.eplus = dummy_var;
          internal_expression.eminus = dummy_var;
          internal_expression.prev_constant = expression_get_constant (cn_expr);

          expression_set_variable (expr, dummy_var, 1.0);

          g_hash_table_insert (solver->marker_vars, constraint, dummy_var);
        }
      else
        {
          Expression *z_row;

          /* Since the constraint is a non-required equality, we need to
           * add error variables around it, i.e. turn it from:
           *
           *   expr = 0
           *
           * to:
           *
           *   expr - eplus + eminus = 0
           */
          solver->slack_counter += 1;

          eplus = variable_new (solver, VARIABLE_SLACK);
          eminus = variable_new (solver, VARIABLE_SLACK);

          expression_set_variable (expr, eplus, -1.0);
          expression_set_variable (expr, eminus, 1.0);

          g_hash_table_insert (solver->marker_vars, constraint, eplus);

          z_row = g_hash_table_lookup (solver->rows, solver->objective);
          expression_set_variable (z_row, eplus, constraint->strength);
          expression_set_variable (z_row, eminus, constraint->strength);
          simplex_solver_add_variable (solver, eplus, solver->objective);
          simplex_solver_add_variable (solver, eminus, solver->objective);

          simplex_solver_insert_error_variable (solver, constraint, eplus);
          simplex_solver_insert_error_variable (solver, constraint, eminus);

          if (constraint_is_stay (constraint))
            {
              g_ptr_array_add (solver->stay_plus_error_vars, eplus);
              g_ptr_array_add (solver->stay_minus_error_vars, eminus);
            }
          else if (constraint_is_edit (constraint))
            {
              internal_expression.eplus = eplus;
              internal_expression.eminus = eminus;
              internal_expression.prev_constant = expression_get_constant (cn_expr);
            }
        }
    }

  return expr;
}

typedef struct {
  Variable *entry;
  double ratio;
  Expression *z_row;
} RatioClosure;

static void
find_ratio (Term *term,
            gpointer data_)
{
  Variable *v = term_get_variable (term);
  double cd = term_get_coefficient (term);
  RatioClosure *data = data_;

  if (cd > 0.0 && variable_is_pivotable (v))
    {
      double zc = expression_get_coefficient (data->z_row, v);
      double r = 0.0;

      if (!approx_val (cd, 0.0))
        r = zc / cd;

      if (r < data->ratio ||
          (approx_val (r, data->ratio) && data->entry != NULL && v < data->entry))
        {
          data->entry = v;
          data->ratio = r;
        }
    }
}

static void
simplex_solver_dual_optimize (SimplexSolver *solver)
{
  Expression *z_row = g_hash_table_lookup (solver->rows, solver->objective);
  GHashTableIter iter;
  gpointer key_p, value_p;

  g_hash_table_iter_init (&iter, solver->infeasible_rows);
  while (g_hash_table_iter_next (&iter, &key_p, &value_p))
    {
      Variable *entry_var, *exit_var;
      Expression *expr;
      RatioClosure data;

      exit_var = variable_ref (key_p);

      g_hash_table_iter_remove (&iter);

      expr = g_hash_table_lookup (solver->rows, exit_var);
      if (expr == NULL)
        {
          variable_unref (exit_var);
          continue;
        }

      if (expression_get_constant (expr) >= 0.0)
        {
          variable_unref (exit_var);
          continue;
        }

      data.ratio = DBL_MAX;
      data.entry = NULL;
      data.z_row = z_row;
      expression_terms_foreach (expr, find_ratio, &data);

      entry_var = data.entry;

      if (entry_var != NULL && !approx_val (data.ratio, DBL_MAX))
        simplex_solver_pivot (solver, entry_var, exit_var);
    }
}

static void
simplex_solver_delta_edit_constant (SimplexSolver *solver,
                                    double delta,
                                    Variable *plus_error_var,
                                    Variable *minus_error_var)
{
  Expression *plus_expr, *minus_expr;
  GHashTable *column_set;
  GHashTableIter iter;
  gpointer key_p, value_p;
  
  plus_expr = g_hash_table_lookup (solver->rows, plus_error_var);
  if (plus_expr != NULL)
    {
      double new_constant = expression_get_constant (plus_expr) + delta;

      expression_set_constant (plus_expr, new_constant);

      if (new_constant < 0.0)
        g_hash_table_add (solver->infeasible_rows, plus_error_var);

      return;
    }

  minus_expr = g_hash_table_lookup (solver->rows, minus_error_var);
  if (minus_expr != NULL)
    {
      double new_constant = expression_get_constant (minus_expr) - delta;

      expression_set_constant (minus_expr, new_constant);

      if (new_constant < 0.0)
        g_hash_table_add (solver->infeasible_rows, minus_error_var);

      return;
    }

  column_set = g_hash_table_lookup (solver->columns, minus_error_var);
  if (column_set == NULL)
    g_critical ("Columns are unset during delta edit");

  g_hash_table_iter_init (&iter, column_set);
  while (g_hash_table_iter_next (&iter, &key_p, &value_p))
    {
      Variable *basic_var = key_p;
      Expression *expr;
      double c, new_constant;

      expr = g_hash_table_lookup (solver->rows, basic_var);
      c = expression_get_coefficient (expr, minus_error_var);

      new_constant = expression_get_constant (expr) + (c * delta);
      expression_set_constant (expr, new_constant);

      if (variable_is_external (basic_var))
        simplex_solver_update_variable (solver, basic_var);

      if (variable_is_restricted (basic_var) && new_constant < 0.0)
        g_hash_table_add (solver->infeasible_rows, basic_var);
    }
}

typedef struct {
  SimplexSolver *solver;
  Variable *subject;
  bool found_unrestricted;
  bool found_new_restricted;
  Variable *retval;
  double coefficient;
} ChooseClosure;

static void
find_subject_restricted (Term *term,
                         gpointer data_)
{
  Variable *v = term_get_variable (term);
  double c = term_get_coefficient (term);
  ChooseClosure *data = data_;

  if (data->found_unrestricted)
    {
      if (!variable_is_restricted (v))
        {
          if (!simplex_solver_column_has_key (data->solver, v))
            {
              data->retval = v;
              return;
            }
        }
    }
  else
    {
      if (variable_is_restricted (v))
        {
          if (!data->found_new_restricted && !variable_is_dummy (v) && c < 0.0)
            {
              GHashTable *col = g_hash_table_lookup (data->solver->columns, v);

              if (col == NULL ||
                  (g_hash_table_size (col) == 1 &&
                   simplex_solver_column_has_key (data->solver, data->solver->objective)))
                {
                  data->subject = v;
                  data->found_new_restricted = true;
                }
            }
        }
      else
        {
          data->subject = v;
          data->found_unrestricted = true;
        }
    }
}

static void
find_subject_dummy (Term *term,
                    gpointer data_)
{
  Variable *v = term_get_variable (term);
  double c = term_get_coefficient (term);
  ChooseClosure *data = data_;

  if (!variable_is_dummy (v))
    {
      data->retval = v;
      return;
    }

  if (!simplex_solver_column_has_key (data->solver, v))
    {
      data->subject = v;
      data->coefficient = c;
    }
}

static Variable *
simplex_solver_choose_subject (SimplexSolver *solver,
                               Expression *expression)
{
  ChooseClosure data;

  data.solver = solver;
  data.subject = NULL;
  data.retval = NULL;
  data.found_unrestricted = false;
  data.found_new_restricted = false;
  data.coefficient = 0.0;
  expression_terms_foreach (expression, find_subject_restricted, &data);

  if (data.retval != NULL)
    return data.retval;

  if (data.subject != NULL)
    return data.subject;

  data.solver = solver;
  data.subject = NULL;
  data.retval = NULL;
  data.found_unrestricted = false;
  data.found_new_restricted = false;
  data.coefficient = 0.0;
  expression_terms_foreach (expression, find_subject_dummy, &data);

  if (data.retval != NULL)
    return data.retval;

  if (approx_val (expression_get_constant (expression), 0.0))
    {
      g_critical ("Unable to satisfy a required constraint");
      return NULL;
    }

  if (data.coefficient > 0.0)
    expression_times (expression, -1.0);

  return data.subject;
}

static bool
simplex_solver_try_adding_directly (SimplexSolver *solver,
                                    Expression *expression)
{
  Variable *subject;

  subject = simplex_solver_choose_subject (solver, expression);
  if (subject == NULL)
    return false;

  expression_new_subject (expression, subject);
  if (simplex_solver_column_has_key (solver, subject))
    simplex_solver_substitute_out (solver, subject, expression);

  simplex_solver_add_row (solver, subject, expression);

  return true;
}

static void
simplex_solver_add_with_artificial_variable (SimplexSolver *solver,
                                             Expression *expression)
{
  Variable *av, *az;
  Expression *az_row;
  Expression *az_tableau_row;
  Expression *e;
  
  av = variable_new (solver, VARIABLE_SLACK);
  solver->artificial_counter += 1;

  az = variable_new (solver, VARIABLE_OBJECTIVE);
  az_row = expression_clone (expression);
  simplex_solver_add_row (solver, az, az_row);
  simplex_solver_add_row (solver, av, expression);
  simplex_solver_optimize (solver, az);

  az_tableau_row = g_hash_table_lookup (solver->rows, az);
  if (!approx_val (expression_get_constant (az_tableau_row), 0.0))
    {
      simplex_solver_remove_row (solver, az);
      simplex_solver_remove_column (solver, av);

      g_critical ("Unable to satisfy a required constraint");
      return;
    }

  e = g_hash_table_lookup (solver->rows, av);
  if (e != NULL)
    {
      Variable *entry_var;

      if (expression_is_constant (e))
        {
          simplex_solver_remove_row (solver, av);
          simplex_solver_remove_row (solver, az);
          return;
        }

      entry_var = expression_get_pivotable_variable (e);
      simplex_solver_pivot (solver, entry_var, av);
    }

  g_assert (g_hash_table_lookup (solver->rows, av) == NULL);

  simplex_solver_remove_column (solver, av);
  simplex_solver_remove_row (solver, az);
}

void
simplex_solver_add_variable (SimplexSolver *solver,
                             Variable *variable,
                             Variable *subject)
{
  if (subject != NULL)
    simplex_solver_insert_column_variable (solver, variable, subject);
}

void
simplex_solver_remove_variable (SimplexSolver *solver,
                                Variable *variable,
                                Variable *subject)
{
  GHashTable *row_set = g_hash_table_lookup (solver->columns, variable);

  if (row_set != NULL && subject != NULL)
    g_hash_table_remove (row_set, subject);
}

void
simplex_solver_update_variable (SimplexSolver *solver,
                                Variable *variable)
{
  if (variable_is_external (variable))
    g_hash_table_add (solver->external_vars, variable);
}

Variable *
simplex_solver_create_variable (SimplexSolver *solver)
{
  return variable_new (solver, VARIABLE_REGULAR);
}

Expression *
simplex_solver_create_expression (SimplexSolver *solver,
                                  double constant)
{
  return expression_new (solver, constant);
}

static void
simplex_solver_add_constraint_internal (SimplexSolver *solver,
                                        Constraint *constraint)
{
  solver->needs_solving = true;

  if (solver->auto_solve)
    simplex_solver_resolve (solver);
}

static void
update_externals (Term *term,
                  gpointer data)
{
  Variable *variable = term_get_variable (term);
  SimplexSolver *solver = data;

  if (variable_is_external (variable))
    simplex_solver_update_variable (solver, variable);
}

Constraint *
simplex_solver_add_constraint (SimplexSolver *solver,
                               Variable *variable,
                               OperatorType op,
                               Expression *expression,
                               StrengthType strength)
{
  Constraint *res;
  Expression *expr;

  /* Turn:
   *
   *   attr OP expression
   *
   * into:
   *
   *   attr - expression OP 0
   */
  expr = expression_new_from_variable (variable);
  expression_add_expression (expr, expression, -1.0, NULL);
  expression_unref (expression);

  expression_terms_foreach (expression, update_externals, solver);

  res = g_slice_new (Constraint);
  res->solver = solver;
  res->expression = expr;
  res->op_type = op;
  res->strength = strength;
  res->is_stay = false;
  res->is_edit = false;

  simplex_solver_add_constraint_internal (solver, res);

  return res;
}

Constraint *
simplex_solver_add_stay_variable (SimplexSolver *solver,
                                  Variable *variable,
                                  StrengthType strength)
{
  Constraint *res;
  Expression *expr;

  /* Turn stay constraint from:
   *
   *   attr == value
   *
   * into:
   *
   *   attr - value == 0
   */
  expr = expression_new_from_variable (variable);
  expression_plus (expr, variable_get_value (variable) * -1.0);

  res = g_slice_new (Constraint);
  res->solver = solver;
  res->expression = expr;
  res->op_type = OPERATOR_TYPE_EQ;
  res->strength = strength;
  res->is_stay = true;
  res->is_edit = false;

  simplex_solver_add_constraint_internal (solver, res);

  return res;
}

bool
simplex_solver_has_stay_variable (SimplexSolver *solver,
                                  Variable *variable)
{
  return false;
}

Constraint *
simplex_solver_add_edit_variable (SimplexSolver *solver,
                                  Variable *variable,
                                  StrengthType strength)
{
  Constraint *res;

  res = g_slice_new (Constraint);
  res->solver = solver;
  res->expression = expression_new_from_variable (variable);
  res->op_type = OPERATOR_TYPE_EQ;
  res->strength = strength;
  res->is_stay = false;
  res->is_edit = true;

  simplex_solver_add_constraint_internal (solver, res);



  return res;
}

bool
simplex_solver_has_edit_variable (SimplexSolver *solver,
                                  Variable *variable)
{
  return false;
}

void
simplex_solver_remove_constraint (SimplexSolver *solver,
                                  Constraint *constraint)
{
  constraint_unref (constraint);
}

void
simplex_solver_suggest_value (SimplexSolver *solver,
                              Variable *variable,
                              double value)
{
}

void
simplex_solver_resolve (SimplexSolver *solver)
{
  if (!solver->needs_solving)
    return;

  simplex_solver_dual_optimize (solver);
  simplex_solver_set_external_variables (solver);

  g_hash_table_remove_all (solver->infeasible_rows);

  simplex_solver_reset_stay_constraints (solver);

  solver->needs_solving = false;
}
