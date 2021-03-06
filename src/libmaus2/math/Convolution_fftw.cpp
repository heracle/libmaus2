/*
    libmaus2
    Copyright (C) 2016 German Tischler

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <libmaus2/math/Convolution.hpp>

#include <libmaus2/math/binom.hpp>
#include <complex>
#include <cassert>
#include <libmaus2/math/ipow.hpp>
#include <libmaus2/parallel/PosixMutex.hpp>
#include <libmaus2/math/numbits.hpp>

#if defined(LIBMAUS2_HAVE_FFTW)
#include <fftw3.h>
#endif

#if defined(LIBMAUS2_HAVE_FFTW)
struct FFTWConvMemBlock
{
	uint64_t const fftn;
	fftw_complex * A;

	static fftw_complex * allocate(size_t const fftn)
	{
		fftw_complex * A = reinterpret_cast<fftw_complex *>(fftw_malloc(fftn * sizeof(fftw_complex)));
		if ( A )
			return A;
		else
			throw std::bad_alloc();
	}

	FFTWConvMemBlock(size_t const rfftn, bool const rerase = true)
	: fftn(rfftn), A(allocate(fftn))
	{
		if ( rerase )
			erase();
	}

	void erase()
	{
		for ( uint64_t i = 0; i < fftn; ++i )
			A[i][0] = A[i][1] = 0.0;
	}

	~FFTWConvMemBlock()
	{
		fftw_free(A);
	}
};
#endif

#if defined(LIBMAUS2_HAVE_FFTW)
struct Transform
{
	typedef Transform this_type;
	typedef libmaus2::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus2::util::shared_ptr<this_type>::type shared_ptr_type;

	uint64_t const size;
	bool const forward;
	FFTWConvMemBlock CCin;
	FFTWConvMemBlock CCout;
	fftw_plan plan;

	Transform(uint64_t const rsize, bool const rforward) : size(rsize), forward(rforward), CCin(size), CCout(size), plan(fftw_plan_dft_1d(size,CCin.A,CCout.A,forward ? FFTW_FORWARD : FFTW_BACKWARD,0))
	{
	}

	~Transform()
	{
		fftw_destroy_plan(plan);
	}

	void reset()
	{
		CCin.erase();
		CCout.erase();
	}

	void execute()
	{
		fftw_execute(plan);
	}
};

static libmaus2::parallel::PosixMutex planlock;
static std::map < uint64_t, std::vector<Transform::shared_ptr_type> > forwardPlans;
static std::map < uint64_t, std::vector<Transform::shared_ptr_type> > backwardPlans;

static Transform::shared_ptr_type getPlan(uint64_t const size, bool const forward)
{
	std::map < uint64_t, std::vector<Transform::shared_ptr_type> > & plans = forward ? forwardPlans : backwardPlans;
	libmaus2::parallel::ScopePosixMutex slock(planlock);

	if ( plans.find(size) == plans.end() )
		plans[size] = std::vector<Transform::shared_ptr_type>(0);

	std::map < uint64_t, std::vector<Transform::shared_ptr_type> >::iterator it = plans.find(size);
	assert ( it != plans.end() );

	std::vector<Transform::shared_ptr_type> & V = it->second;

	if ( ! V.size() )
	{
		// std::cerr << "allocating plan of size " << size << std::endl;
		V.push_back(Transform::shared_ptr_type(new Transform(size,forward)));
	}

	assert ( V.size() );

	Transform::shared_ptr_type T = V.back();
	V.pop_back();

	assert ( T->size == size );
	assert ( T->forward == forward );

	T->reset();

	return T;
}

static void returnPlan(Transform::shared_ptr_type plan)
{
	bool const forward = plan->forward;
	uint64_t const size = plan->size;

	std::map < uint64_t, std::vector<Transform::shared_ptr_type> > & plans = forward ? forwardPlans : backwardPlans;
	libmaus2::parallel::ScopePosixMutex slock(planlock);

	if ( plans.find(size) == plans.end() )
		plans[size] = std::vector<Transform::shared_ptr_type>(0);

	std::map < uint64_t, std::vector<Transform::shared_ptr_type> >::iterator it = plans.find(size);
	assert ( it != plans.end() );

	std::vector<Transform::shared_ptr_type> & V = it->second;
	V.push_back(plan);
}
#endif

std::vector<double> libmaus2::math::Convolution::convolutionFFTW(
	std::vector<double> const &
		#if defined(LIBMAUS2_HAVE_FFTW)
		RA
		#endif
		,
	std::vector<double> const &
		#if defined(LIBMAUS2_HAVE_FFTW)
		RB
		#endif
)
{
	#if defined(LIBMAUS2_HAVE_FFTW)
	uint64_t const ra = RA.size();
	uint64_t const rb = RB.size();

	if ( ra*rb == 0 )
		return std::vector<double>(0);

	uint64_t const rfftn = (ra+rb-1);
	uint64_t const minsize = 4096;
	uint64_t const fftn = std::max(minsize,libmaus2::math::nextTwoPow(rfftn));

	Transform::shared_ptr_type planA = getPlan(fftn,true);
	Transform::shared_ptr_type planB = getPlan(fftn,true);
	Transform::shared_ptr_type planR = getPlan(fftn,false);

	FFTWConvMemBlock & CCin_A = planA->CCin;
	FFTWConvMemBlock & CCtmp_A = planA->CCout;

	FFTWConvMemBlock & CCin_B = planB->CCin;
	FFTWConvMemBlock & CCtmp_B = planB->CCout;

	FFTWConvMemBlock & CCtmp_C = planR->CCin;
	FFTWConvMemBlock & CCout = planR->CCout;

	for ( uint64_t i = 0; i < RA.size(); ++i )
		CCin_A.A[i][0] = RA[i];
	for ( uint64_t i = 0; i < RB.size(); ++i )
		CCin_B.A[i][0] = RB[i];

	planA->execute();
	planB->execute();

	for ( uint64_t i = 0; i < fftn; ++i )
	{
		std::complex<double> CA(CCtmp_A.A[i][0],CCtmp_A.A[i][1]);
		std::complex<double> CB(CCtmp_B.A[i][0],CCtmp_B.A[i][1]);
		std::complex<double> CC = CA * CB;
		CCtmp_C.A[i][0] = CC.real();
		CCtmp_C.A[i][1] = CC.imag();
	}

	planR->execute();

	std::vector<double> R(rfftn);
	for ( uint64_t i = 0; i < rfftn; ++i )
		R[i] = CCout.A[i][0] / fftn;

	returnPlan(planA);
	returnPlan(planB);
	returnPlan(planR);

	return R;
	#else
	libmaus2::exception::LibMausException lme;
	lme.getStream() << "[E] libmaus2::math::Convolution::convolutionFFTW: libmaus2 is built without fftw support" << std::endl;
	lme.finish();
	throw lme;
	#endif
}

void libmaus2::math::Convolution::cleanup()
{
	#if defined(LIBMAUS2_HAVE_FFTW)
	libmaus2::parallel::ScopePosixMutex slock(planlock);
	forwardPlans.clear();
	backwardPlans.clear();
	#endif
}
