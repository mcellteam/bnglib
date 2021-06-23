/******************************************************************************
 * Copyright (C) 2020-2021 by
 * The Salk Institute for Biological Studies
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
******************************************************************************/

#include <iostream>
#include <sstream>

#include "bng/bng_engine.h"
#include "bng/bngl_names.h"

#define BOOST_POOL_NO_MT
#include <boost/pool/pool_alloc.hpp>
#undef BOOST_POOL_NO_MT

using namespace std;

namespace BNG {


void BNGEngine::initialize() {
  // insert information on rxn rules into rxn container
  for (const RxnRule& r: data.get_rxn_rules()) {
    all_rxns.add_and_finalize(r);
  }
}

string BNGEngine::get_stats_report() const {
  stringstream res;

  std::set<reactant_class_id_t> active_reactant_classes;
  uint num_active_species = 0;
  for (const Species* s: all_species.get_species_vector()) {
    release_assert(s != nullptr);
    if (s->was_instantiated()) {
      num_active_species++;
      if (s->has_valid_reactant_class_id()) {
        active_reactant_classes.insert(s->get_reactant_class_id());
      }
    }
  }

  res << "[" <<
      "active/total species " << num_active_species << "/" << all_species.get_species_vector().size() <<
      ", rxn classes " << all_rxns.get_num_rxn_classes() <<
      ", active/total reactant classes " << active_reactant_classes.size() << "/" << all_rxns.get_num_existing_reactant_classes() <<
      "]";
  return res.str();
}


Cplx BNGEngine::create_cplx_from_species(
    const species_id_t id, const orientation_t o, const compartment_id_t compartment_id) const {
  const Cplx& ref = all_species.get(id);
  Cplx copy = ref;
  copy.set_orientation(o);
  copy.set_compartment_id(compartment_id);
  return copy;
}



std::string BNGEngine::export_to_bngl(
    std::ostream& out_parameters,
    std::ostream& out_molecule_types,
    std::ostream& out_compartments,
    std::ostream& out_reaction_rules,
    const bool rates_for_nfsim,
    const double volume_um3_for_nfsim,
    const double area_um3_for_nfsim) const {

  if (rates_for_nfsim) {
    out_parameters << IND << PARAM_RATE_CONV_VOLUME << " " << f_to_str(volume_um3_for_nfsim) << " * 1e-15 # compartment volume in litres\n";
  }
  else {
    out_parameters << IND << PARAM_RATE_CONV_VOLUME << " 1e-15 # um^3 to litres\n";
  }

  export_molecule_types_as_bngl(out_parameters, out_molecule_types);

  string err_msg = export_reaction_rules_as_bngl(out_parameters, out_reaction_rules);

  err_msg += export_compartments_as_bngl(out_parameters, out_compartments);

  return err_msg;
}


void BNGEngine::export_molecule_types_as_bngl(std::ostream& out_parameters, std::ostream& out_molecule_types) const {
  out_molecule_types << BEGIN_MOLECULE_TYPES << "\n";

  out_parameters << "\n" << BNG::IND << "# diffusion constants\n";
  for (const ElemMolType& mt: data.get_elem_mol_types()) {
    if (mt.is_reactive_surface() || is_species_superclass(mt.name)) {
      continue;
    }

    // define as mol type
    out_molecule_types << IND << mt.to_str(data) << "\n";

    // and also set its diffusion constant as parameter
    if (mt.is_vol()) {
      out_parameters << IND << MCELL_DIFFUSION_CONSTANT_3D_PREFIX;
    }
    else if (mt.is_surf()){
      out_parameters << IND << MCELL_DIFFUSION_CONSTANT_2D_PREFIX;
    }
    out_parameters << mt.name << " " << f_to_str(mt.D) << "\n";
  }

  out_molecule_types << END_MOLECULE_TYPES << "\n";
}


std::string BNGEngine::export_reaction_rules_as_bngl(
    std::ostream& out_parameters,
    std::ostream& out_reaction_rules) const {
  out_reaction_rules << BEGIN_REACTION_RULES << "\n";

  out_parameters << "\n" << BNG::IND << "# parameters to control rates in MCell and BioNetGen\n";
  out_parameters << IND << PARAM_MCELL2BNG_VOL_CONV << " " << NA_VALUE_STR << " * " << PARAM_RATE_CONV_VOLUME << "\n";
  out_parameters << IND << PARAM_VOL_RXN << " 1\n";
  out_parameters << IND << MCELL_REDEFINE_PREFIX << PARAM_VOL_RXN << " " << PARAM_MCELL2BNG_VOL_CONV << "\n";
  out_parameters << "\n" << BNG::IND << "# reaction rates\n";

  for (size_t i = 0; i < get_all_rxns().get_rxn_rules_vector().size(); i++) {
    const RxnRule* rr = get_all_rxns().get_rxn_rules_vector()[i];

    string rxn_as_bngl = rr->to_str(false, false, false);

    string rate_param = "k" + to_string(i);
    out_parameters << IND << rate_param << " " << f_to_str(rr->base_rate_constant);
    if (rr->is_bimol_vol_rxn()) {
      out_parameters  << " / " << PARAM_MCELL2BNG_VOL_CONV << " * " << PARAM_VOL_RXN;
    }
    else if (rr->is_surf_rxn() || rr->is_reactive_surface_rxn()) {
      // TODO: only error msg and continue
      return "Export of surface reactions to BNGL is not supported yet, error for " + rxn_as_bngl + ".";
    }
    out_parameters << "\n";


    out_reaction_rules << IND << rxn_as_bngl;
    out_reaction_rules << " " << rate_param << "\n";
  }

  out_reaction_rules << END_REACTION_RULES << "\n";
  return "";
}


std::string BNGEngine::export_compartments_as_bngl(
    std::ostream& out_parameters,
    std::ostream& out_compartments) const {

  out_compartments << BEGIN_COMPARTMENTS << "\n";

  for (const Compartment& comp: data.get_compartments()) {
    if (comp.name == DEFAULT_COMPARTMENT_NAME) {
      // ignored
      continue;
    }

    if (comp.is_3d) {
      string vol_name = PREFIX_VOLUME + comp.name;

      out_parameters << IND << vol_name << " " <<
          f_to_str(comp.get_volume_or_area()) << " # um^3\n";

      out_compartments << IND << comp.name << " 3 " << vol_name;
    }
    else {
      string area_name = PREFIX_AREA + comp.name;

      out_parameters << IND << area_name << " " <<
          f_to_str(comp.get_volume_or_area()) << " # um^2\n";

      out_compartments << IND << comp.name << " 2 " << area_name << " * " << PARAM_THICKNESS;
    }

    if (comp.parent_compartment_id != COMPARTMENT_ID_INVALID) {
      out_compartments << " " << data.get_compartment(comp.parent_compartment_id).name << "\n";
    }
    else {
      out_compartments << "\n";
    }
  }

  out_compartments << END_COMPARTMENTS << "\n";

  return "";
}

} // namespace BNG
