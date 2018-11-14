// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.

  Consult the COPYING file in the top-level source directory of this
  module for the precise wording of the license and the list of
  copyright holders.
*/
/*!
 * \file
 *
 * \copydoc Ewoms::EclNewtonMethod
 */
#ifndef EWOMS_ECL_NEWTON_METHOD_HH
#define EWOMS_ECL_NEWTON_METHOD_HH

#include <ewoms/models/blackoil/blackoilnewtonmethod.hh>
#include <ewoms/common/signum.hh>

#include <opm/material/common/Unused.hpp>

BEGIN_PROPERTIES

NEW_PROP_TAG(NewtonSumTolerance);

END_PROPERTIES

namespace Ewoms {

/*!
 * \brief A newton solver which is ebos specific.
 */
template <class TypeTag>
class EclNewtonMethod : public BlackOilNewtonMethod<TypeTag>
{
    typedef BlackOilNewtonMethod<TypeTag> ParentType;
    typedef typename GET_PROP_TYPE(TypeTag, DiscNewtonMethod) DiscNewtonMethod;

    typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
    typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;
    typedef typename GET_PROP_TYPE(TypeTag, SolutionVector) SolutionVector;
    typedef typename GET_PROP_TYPE(TypeTag, GlobalEqVector) GlobalEqVector;
    typedef typename GET_PROP_TYPE(TypeTag, PrimaryVariables) PrimaryVariables;
    typedef typename GET_PROP_TYPE(TypeTag, EqVector) EqVector;
    typedef typename GET_PROP_TYPE(TypeTag, Indices) Indices;
    typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
    typedef typename GET_PROP_TYPE(TypeTag, Linearizer) Linearizer;
    typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;

    static const unsigned numEq = GET_PROP_VALUE(TypeTag, NumEq);

    static constexpr int contiSolventEqIdx = Indices::contiSolventEqIdx;
    static constexpr int contiPolymerEqIdx = Indices::contiPolymerEqIdx;
    static constexpr int contiEnergyEqIdx = Indices::contiEnergyEqIdx;

    friend NewtonMethod<TypeTag>;
    friend DiscNewtonMethod;
    friend ParentType;

public:
    EclNewtonMethod(Simulator& simulator) : ParentType(simulator)
    {
        errorPvFraction_ = 1.0;
        sumTolerance_ = EWOMS_GET_PARAM(TypeTag, Scalar, NewtonSumTolerance);
        relaxedTolerance_ = 1e9;
    }

    /*!
     * \brief Register all run-time parameters for the Newton method.
     */
    static void registerParameters()
    {
        ParentType::registerParameters();

        EWOMS_REGISTER_PARAM(TypeTag, Scalar, NewtonSumTolerance,
                             "The maximum error tolerated by the Newton"
                             "method for considering a solution to be "
                             "converged");
    }

    /*!
     * \brief Returns true if the error of the solution is below the
     *        tolerance.
     */
    bool converged() const
    {
        if (errorPvFraction_ < 0.03)
            return (this->error_ < relaxedTolerance_ && errorSum_ < sumTolerance_) ;
        else if (this->numIterations() > 8)
            return (this->error_ < relaxedTolerance_ && errorSum_ < sumTolerance_) ;

        return this->error_ <= this->tolerance() && errorSum_ <= sumTolerance_;
    }

    void preSolve_(const SolutionVector& currentSolution  OPM_UNUSED,
                   const GlobalEqVector& currentResidual)
    {
        const auto& constraintsMap = this->model().linearizer().constraintsMap();
        this->lastError_ = this->error_;
        Scalar newtonMaxError = EWOMS_GET_PARAM(TypeTag, Scalar, NewtonMaxError);

        // calculate the error as the maximum weighted tolerance of
        // the solution's residual
        this->error_ = 0.0;
        Dune::FieldVector<Scalar, numEq> componentSumError;
        std::fill(componentSumError.begin(), componentSumError.end(), 0.0);
        Scalar sumPv = 0.0;
        errorPvFraction_ = 0.0;
        const Scalar dt = this->simulator_.timeStepSize();
        for (unsigned dofIdx = 0; dofIdx < currentResidual.size(); ++dofIdx) {
            // do not consider auxiliary DOFs for the error
            if (dofIdx >= this->model().numGridDof()
                || this->model().dofTotalVolume(dofIdx) <= 0.0)
                continue;

            if (!this->model().isLocalDof(dofIdx))
                continue;

            // also do not consider DOFs which are constraint
            if (this->enableConstraints_()) {
                if (constraintsMap.count(dofIdx) > 0)
                    continue;
            }

            const auto& r = currentResidual[dofIdx];
            const double pvValue =
                this->simulator_.problem().porosity(dofIdx)
                * this->model().dofTotalVolume(dofIdx);
            sumPv += pvValue;
            bool cnvViolated = false;

            for (unsigned eqIdx = 0; eqIdx < r.size(); ++eqIdx) {
                Scalar tmpError = r[eqIdx] * dt * this->model().eqWeight(dofIdx, eqIdx) / pvValue;
                Scalar tmpError2 = r[eqIdx] * this->model().eqWeight(dofIdx, eqIdx);

                this->error_ = Opm::max(std::abs(tmpError), this->error_);

                if (std::abs(tmpError) > this->tolerance_)
                    cnvViolated = true;

                componentSumError[eqIdx] += std::abs(tmpError2);
            }
            if (cnvViolated)
                errorPvFraction_ += pvValue;
        }

        // take the other processes into account
        this->error_ = this->comm_.max(this->error_);
        componentSumError = this->comm_.sum(componentSumError);
        sumPv = this->comm_.sum(sumPv);
        errorPvFraction_ = this->comm_.sum(errorPvFraction_);

        componentSumError /= sumPv;
        componentSumError *= dt;

        errorPvFraction_ /= sumPv;

        errorSum_ = 0;
        for (unsigned eqIdx = 0; eqIdx < numEq; ++eqIdx)
            errorSum_ = std::max(std::abs(componentSumError[eqIdx]), errorSum_);

        // update the sum tolerance: larger reservoirs can tolerate a higher amount of
        // mass lost per time step than smaller ones! since this is not linear, we use
        // the cube root of the overall pore volume, i.e., the value specified by the
        // NewtonSumTolerance parameter is the "incorrect" mass per timestep for an
        // reservoir that exhibits 1 m^3 of pore volume. A reservoir with a total pore
        // volume of 10^3 m^3 will tolerate 10 times as much.
        sumTolerance_ =
            EWOMS_GET_PARAM(TypeTag, Scalar, NewtonSumTolerance)
            * std::cbrt(sumPv);

        // make sure that the error never grows beyond the maximum
        // allowed one
        if (this->error_ > newtonMaxError)
            throw Opm::NumericalIssue("Newton: Error "+std::to_string(double(this->error_))
                                        +" is larger than maximum allowed error of "
                                        +std::to_string(double(newtonMaxError)));

        // make sure that the error never grows beyond the maximum
        // allowed one
        if (errorSum_ > newtonMaxError)
            throw Opm::NumericalIssue("Newton: Sum of the error "+std::to_string(double(errorSum_))
                                        +" is larger than maximum allowed error of "
                                        +std::to_string(double(newtonMaxError)));
    }

private:
    Scalar errorPvFraction_;
    Scalar errorSum_;

    Scalar relaxedTolerance_;

    Scalar sumTolerance_;
};
} // namespace Ewoms

#endif
