/* Copyright © 2017-2020 ABBYY Production LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
--------------------------------------------------------------------------------------------------------------*/

#include <common.h>
#pragma hdrstop

#include <NeoML/TraditionalML/GradientBoost.h>
#include <NeoML/TraditionalML/GradientBoostQuickScorer.h>
#include <GradientBoostModel.h>
#include <RegressionTree.h>
#include <GradientBoostFullProblem.h>
#include <GradientBoostFastHistProblem.h>
#include <GradientBoostFullTreeBuilder.h>
#include <GradientBoostFastHistTreeBuilder.h>
#include <ProblemWrappers.h>
#include <NeoMathEngine/OpenMP.h>

namespace NeoML {

const double MaxExpArgument = 30; // the maximum argument for an exponent

IGradientBoostModel::~IGradientBoostModel()
{
}

IGradientBoostRegressionModel::~IGradientBoostRegressionModel()
{
}

IRegressionTreeNode::~IRegressionTreeNode()
{
}

// Loss function interface
class IGradientBoostingLossFunction : public virtual IObject {
public:
	// Calculates function gradient
	virtual void CalcGradientAndHessian( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers,
		CArray< CArray<double> >& gradient, CArray< CArray<double> >& hessian ) const = 0;

	// Calculates loss
	virtual double CalcLossMean( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers ) const = 0;
};

//------------------------------------------------------------------------------------------------------------

// Binomial loss function
class CGradientBoostingBinomialLossFunction : public IGradientBoostingLossFunction {
public:
	// IGradientBoostingLossFunction interface methods
	void CalcGradientAndHessian( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers,
		CArray< CArray<double> >& gradient, CArray< CArray<double> >& hessian ) const override;

	double CalcLossMean( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers ) const override;
};

void CGradientBoostingBinomialLossFunction::CalcGradientAndHessian( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers, CArray< CArray<double> >& gradients, CArray< CArray<double> >& hessians ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	gradients.SetSize( predicts.Size() );
	hessians.SetSize( predicts.Size() );

	for( int i = 0; i < predicts.Size(); i++ ) {
		gradients[i].SetSize( predicts[i].Size() );
		hessians[i].SetSize( predicts[i].Size() );
		for( int j = 0; j < predicts[i].Size(); j++ ) {
			const double pred = 1.0f / ( 1.0f + exp( min( -predicts[i][j], MaxExpArgument ) ) );
			gradients[i][j] = static_cast<double>( pred - answers[i][j] );
			hessians[i][j] = static_cast<double>( max( pred * ( 1.0 - pred ), 1e-16 ) );
		}
	}
}

double CGradientBoostingBinomialLossFunction::CalcLossMean( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	double overallSum = 0;
	auto getMean = []( double sum, int n ) { return n != 0 ? sum / static_cast<double>( n ) : 0; };
	for( int i = 0; i < predicts.Size(); ++i ) {
		double sum = 0;
		for( int j = 0; j < predicts[i].Size(); ++j ) {
			sum += log( 1 + exp( min( -predicts[i][j], MaxExpArgument ) ) ) - predicts[i][j] * answers[i][j]; 
		}
		overallSum += getMean( sum, predicts[i].Size() );
	}

	return getMean( overallSum, predicts.Size() );
}

//------------------------------------------------------------------------------------------------------------

// Exponential loss function (similar to AdaBoost)
class CGradientBoostingExponentialLossFunction : public IGradientBoostingLossFunction {
public:
	// IGradientBoostingLossFunction interface methods
	void CalcGradientAndHessian( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers,
		CArray< CArray<double> >& gradient, CArray< CArray<double> >& hessian ) const override;

	double CalcLossMean( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers ) const override;
};

void CGradientBoostingExponentialLossFunction::CalcGradientAndHessian( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers, CArray< CArray<double> >& gradients, CArray< CArray<double> >& hessians ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	gradients.SetSize( predicts.Size() );
	hessians.SetSize( predicts.Size() );

	for( int i = 0; i < predicts.Size(); i++ ) {
		gradients[i].SetSize( predicts[i].Size() );
		hessians[i].SetSize( predicts[i].Size() );
		for( int j = 0; j < predicts[i].Size(); j++ ) {
			const double temp = -( 2 * answers[i][j] - 1 );
			const double tempExp = exp( min( temp * predicts[i][j], MaxExpArgument ) );
			gradients[i][j] = static_cast<double>( temp * tempExp );
			hessians[i][j] = static_cast<double>( temp * temp * tempExp );
		}
	}
}

double CGradientBoostingExponentialLossFunction::CalcLossMean( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	double overallSum = 0;
	auto getMean = []( double sum, int n ) { return n != 0 ? sum / static_cast<double>( n ) : 0; };
	for( int i = 0; i < predicts.Size(); ++i ) {
		double sum = 0;
		for( int j = 0; j < predicts[i].Size(); ++j ) {
			sum += exp( min( ( 1.0 - 2.0 * answers[i][j] ) * predicts[i][j], MaxExpArgument ) );
		}
		overallSum += getMean( sum, predicts[i].Size() );
	}

	return getMean( overallSum, predicts.Size() );
}

//------------------------------------------------------------------------------------------------------------

// Smoothed square hinge function
class CGradientBoostingSquaredHinge : public IGradientBoostingLossFunction {
public:
	// IGradientBoostingLossFunction interface methods
	void CalcGradientAndHessian( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers,
		CArray< CArray<double> >& gradient, CArray< CArray<double> >& hessian ) const override;

	double CalcLossMean( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers ) const override;
};

void CGradientBoostingSquaredHinge::CalcGradientAndHessian( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers, CArray< CArray<double> >& gradients, CArray< CArray<double> >& hessians ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	gradients.SetSize( predicts.Size() );
	hessians.SetSize( predicts.Size() );

	for( int i = 0; i < predicts.Size(); i++ ) {
		gradients[i].SetSize( predicts[i].Size() );
		hessians[i].SetSize( predicts[i].Size() );
		for( int j = 0; j < predicts[i].Size(); j++ ) {
			const double t = -( 2 * answers[i][j] - 1 );

			if( t * predicts[i][j] < 1 ) {
				gradients[i][j] = static_cast<double>( 2 * t * ( t * predicts[i][j] - 1 ) );
				hessians[i][j] = static_cast<double>( 2 * t * t );
			} else {
				gradients[i][j] = 0.0;
				hessians[i][j] = 1e-16;
			}
		}
	}
}

double CGradientBoostingSquaredHinge::CalcLossMean( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	double overallSum = 0;
	auto getMean = []( double sum, int n ) { return n != 0 ? sum / static_cast<double>( n ) : 0; };
	for( int i = 0; i < predicts.Size(); ++i ) {
		double sum = 0;
		for( int j = 0; j < predicts[i].Size(); ++j ) {
			const double base = max( 0.0, 1.0 - ( 2.0 * answers[i][j] - 1.0 ) * predicts[i][j] );
			sum += base * base;
		}
		overallSum += getMean( sum, predicts[i].Size() );
	}

	return getMean( overallSum, predicts.Size() );
}

//------------------------------------------------------------------------------------------------------------

// Quadratic loss function for classification and regression
class CGradientBoostingSquareLoss : public IGradientBoostingLossFunction {
public:
	// IGradientBoostingLossFunction interface methods
	void CalcGradientAndHessian( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers,
		CArray< CArray<double> >& gradient, CArray< CArray<double> >& hessian ) const override;

	double CalcLossMean( const CArray< CArray<double> >& predicts, const CArray< CArray<double> >& answers ) const override;
};

void CGradientBoostingSquareLoss::CalcGradientAndHessian( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers, CArray< CArray<double> >& gradients, CArray< CArray<double> >& hessians ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	gradients.SetSize( predicts.Size() );
	hessians.SetSize( predicts.Size() );

	for( int i = 0; i < predicts.Size(); i++ ) {
		gradients[i].SetSize( predicts[i].Size() );
		hessians[i].SetSize( predicts[i].Size() );
		for( int j = 0; j < predicts[i].Size(); j++ ) {
			gradients[i][j] = static_cast<double>( predicts[i][j] - answers[i][j] );
			hessians[i][j] = static_cast<double>( 1.0 );
		}
	}
}

double CGradientBoostingSquareLoss::CalcLossMean( const CArray< CArray<double> >& predicts,
	const CArray< CArray<double> >& answers ) const
{
	NeoAssert( predicts.Size() == answers.Size() );

	double overallSum = 0;
	auto getMean = []( double sum, int n ) { return n != 0 ? sum / static_cast<double>( n ) : 0; };
	for( int i = 0; i < predicts.Size(); ++i ) {
		double sum = 0;
		for( int j = 0; j < predicts[i].Size(); ++j ) {
			const double diff = answers[i][j] - predicts[i][j];
			sum += diff * diff / 2.0;
		}
		overallSum += getMean( sum, predicts[i].Size() );
	}

	return getMean( overallSum, predicts.Size() );
}

//------------------------------------------------------------------------------------------------------------

// Generates an array of random K numbers in the [0, N) range
static void generateRandomArray( CRandom& random, int n, int k, CArray<int>& result )
{
	NeoAssert( k <= n );
	NeoAssert( 1 <= k );

	result.Empty();
	result.SetBufferSize( n );
	for( int i = 0; i < n; i++ ) {
		result.Add( i );
	}

	if( k == n ) {
		return;
	}

	for( int i = 0; i < k; i++ ) {
		// Choose a random number from [i, n - 1] range
		const int index = random.UniformInt( i, n - 1 );
		swap( result[i], result[index] );
	}
	result.SetSize( k );
	result.QuickSort< Ascending<int> >();
}

//------------------------------------------------------------------------------------------------------------

#if FINE_PLATFORM( FINE_IOS )
	// No OpenMP available for iOS, so working in one thread
	static inline CGradientBoost::CParams processParams( const CGradientBoost::CParams& params )
	{
		CGradientBoost::CParams result = params;
		result.ThreadCount = 1;
		return result;
	}
#elif FINE_PLATFORM( FINE_WINDOWS ) || FINE_PLATFORM( FINE_LINUX ) || FINE_PLATFORM( FINE_ANDROID ) || FINE_PLATFORM( FINE_DARWIN )
	static inline CGradientBoost::CParams processParams( const CGradientBoost::CParams& params ) { return params; }
#else
	#error Unknown platform
#endif

CGradientBoost::CGradientBoost( const CParams& _params ) :
	params( processParams( _params ) ),
	logStream( 0 ),
	loss( 0 )
{
	NeoAssert( params.IterationsCount > 0 );
	NeoAssert( 0 <= params.Subsample && params.Subsample <= 1 );
	NeoAssert( 0 <= params.Subfeature && params.Subfeature <= 1 );
	NeoAssert( params.MaxTreeDepth >= 0 );
	NeoAssert( params.MaxNodesCount >= 0 || params.MaxNodesCount == NotFound );
	NeoAssert( params.PruneCriterionValue >= 0 );
	NeoAssert( params.ThreadCount > 0 );
	NeoAssert( params.MinSubsetWeight >= 0 );
}

CGradientBoost::~CGradientBoost()
{
}

CPtr<IMultivariateRegressionModel> CGradientBoost::TrainRegression(
	const IMultivariateRegressionProblem& problem )
{
	while( !TrainStep( problem ) ) {};
	return GetMultivariateRegressionModel( problem );
}

CPtr<IRegressionModel> CGradientBoost::TrainRegression( const IRegressionProblem& problem )
{
	while( !TrainStep( problem ) ) {};
	return GetRegressionModel( problem );
}

CPtr<IModel> CGradientBoost::Train( const IProblem& problem )
{
	while( !TrainStep( problem ) ) {};
	return GetClassificationModel( problem );
}

// Creates a tree builder depending on the problem type
void CGradientBoost::createTreeBuilder( const IMultivariateRegressionProblem* problem )
{
	switch( params.TreeBuilder ) {
		case GBTB_Full:
		case GBTB_MultiFull:
		{
			CGradientBoostFullTreeBuilderParams builderParams;
			builderParams.L1RegFactor = params.L1RegFactor;
			builderParams.L2RegFactor = params.L2RegFactor;
			builderParams.MinSubsetHessian = 1e-3f;
			builderParams.ThreadCount = params.ThreadCount;
			builderParams.MaxTreeDepth = params.MaxTreeDepth;
			builderParams.MaxNodesCount = params.MaxNodesCount;
			builderParams.PruneCriterionValue = params.PruneCriterionValue;
			builderParams.MinSubsetWeight = params.MinSubsetWeight;
			builderParams.DenseTreeBoostCoefficient = params.DenseTreeBoostCoefficient;
			if( params.TreeBuilder == GBTB_MultiFull ) {
				fullMultiClassTreeBuilder = FINE_DEBUG_NEW CGradientBoostFullTreeBuilder<CGradientBoostStatisticsMulti>( builderParams, logStream );
			} else {
				fullSingleClassTreeBuilder = FINE_DEBUG_NEW CGradientBoostFullTreeBuilder<CGradientBoostStatisticsSingle>( builderParams, logStream );
			}
			fullProblem = FINE_DEBUG_NEW CGradientBoostFullProblem( params.ThreadCount, problem,
				usedVectors, usedFeatures, featureNumbers );
			break;
		}
		case GBTB_FastHist:
		case GBTB_MultiFastHist:
		{
			CGradientBoostFastHistTreeBuilderParams builderParams;
			builderParams.L1RegFactor = params.L1RegFactor;
			builderParams.L2RegFactor = params.L2RegFactor;
			builderParams.MinSubsetHessian = 1e-3f;
			builderParams.ThreadCount = params.ThreadCount;
			builderParams.MaxTreeDepth = params.MaxTreeDepth;
			builderParams.MaxNodesCount = params.MaxNodesCount;
			builderParams.PruneCriterionValue = params.PruneCriterionValue;
			builderParams.MaxBins = params.MaxBins;
			builderParams.MinSubsetWeight = params.MinSubsetWeight;
			builderParams.DenseTreeBoostCoefficient = params.DenseTreeBoostCoefficient;
			if( params.TreeBuilder == GBTB_MultiFastHist ) {
				fastHistMultiClassTreeBuilder = FINE_DEBUG_NEW CGradientBoostFastHistTreeBuilder<CGradientBoostStatisticsMulti>( builderParams, logStream, problem->GetValueSize() );
			} else {
				fastHistSingleClassTreeBuilder = FINE_DEBUG_NEW CGradientBoostFastHistTreeBuilder<CGradientBoostStatisticsSingle>( builderParams, logStream, 1 );
			}
			fastHistProblem = FINE_DEBUG_NEW CGradientBoostFastHistProblem( params.ThreadCount, params.MaxBins,
				*problem, usedVectors, usedFeatures );
			break;
		}
		default:
			NeoAssert( false );
	}
}

// Destroys a tree builder
void CGradientBoost::destroyTreeBuilder()
{
	fullSingleClassTreeBuilder.Release();
	fullMultiClassTreeBuilder.Release();
	fullProblem.Release();
	fastHistSingleClassTreeBuilder.Release();
	fastHistMultiClassTreeBuilder.Release();
	fastHistProblem.Release();
	baseProblem.Release();
}

// Creates a loss function based on CParam.LossFunction
CPtr<IGradientBoostingLossFunction> CGradientBoost::createLossFunction() const
{
	switch( params.LossFunction ) {
		case LF_Binomial:
			return FINE_DEBUG_NEW CGradientBoostingBinomialLossFunction();
			break;
		case LF_Exponential:
			return FINE_DEBUG_NEW CGradientBoostingExponentialLossFunction();
			break;
		case LF_SquaredHinge:
			return FINE_DEBUG_NEW CGradientBoostingSquaredHinge();
			break;
		case LF_L2:
			return FINE_DEBUG_NEW CGradientBoostingSquareLoss();
			break;
		default:
			NeoAssert( false );
			return 0;
	}
}

// Initializes the algorithm
void CGradientBoost::initialize()
{
	const int modelCount = baseProblem->GetValueSize();
	const int vectorCount = baseProblem->GetVectorCount();
	const int featureCount = baseProblem->GetFeatureCount();

	NeoAssert( modelCount >= 1 );
	NeoAssert( vectorCount > 0 );
	NeoAssert( featureCount > 0 );

	lossFunction = createLossFunction();
	models.SetSize( isMultiTreesModel() ? 1 : modelCount );

	if( predictCache.Size() == 0 ) {
		predictCache.SetSize( modelCount );
		CPredictionCacheItem item;
		item.Step = 0;
		item.Value = 0;
		for( int i = 0; i < predictCache.Size(); i++ ) {
			predictCache[i].Add( item, vectorCount );
		}
	}

	predicts.SetSize( modelCount );
	answers.SetSize( modelCount );
	gradients.SetSize( modelCount );
	hessians.SetSize( modelCount );
	if( params.Subsample == 1.0 ) {
		usedVectors.DeleteAll();
		for( int i = 0; i < vectorCount; i++ ) {
			usedVectors.Add( i );
		}
	}
	if( params.Subfeature == 1.0 ) {
		usedFeatures.DeleteAll();
		featureNumbers.DeleteAll();
		for(int i = 0; i < featureCount; i++ ) {
			usedFeatures.Add( i );
			featureNumbers.Add( i );
		}
	}

	try {
		createTreeBuilder( baseProblem );
	} catch( ... ) {
		destroyTreeBuilder(); // return to the initial state
		throw;
	}
	
	if( fullProblem != nullptr && params.Subfeature == 1.0 && params.Subsample == 1.0 ) {
		fullProblem->Update();
	}
}

// Performs gradient boosting iteration
// On a sub-problem of the first problem using cache
void CGradientBoost::executeStep( IGradientBoostingLossFunction& lossFunction,
	const IMultivariateRegressionProblem* problem, CObjectArray<IRegressionTreeNode>& curModels )
{
	NeoAssert( !models.IsEmpty() );
	NeoAssert( curModels.IsEmpty() );
	NeoAssert( problem != nullptr );

	const int vectorCount = problem->GetVectorCount();
	const int featureCount = problem->GetFeatureCount();

	if( params.Subsample < 1.0 ) {
		generateRandomArray( params.Random != nullptr ? *params.Random : defaultRandom, vectorCount,
			max( static_cast<int>( vectorCount * params.Subsample ), 1 ), usedVectors );
	}
	if( params.Subfeature < 1.0 ) {
		generateRandomArray( params.Random != nullptr ? *params.Random : defaultRandom, featureCount,
			max( static_cast<int>( featureCount * params.Subfeature ), 1 ), usedFeatures );
		
		if( featureNumbers.Size() != featureCount ) {
			featureNumbers.SetSize( featureCount );
		}
		for( int i = 0; i < featureCount; ++i ) {
			featureNumbers[i] = NotFound;
		}
		for( int i = 0; i < usedFeatures.Size(); ++i ) {
			featureNumbers[usedFeatures[i]] = i;
		}
	}

	const int curStep = models[0].Size();

	for( int i = 0; i < predicts.Size(); i++ ) {
		predicts[i].SetSize( usedVectors.Size() );
		answers[i].SetSize( usedVectors.Size() );
		gradients[i].Empty();
		hessians[i].Empty();
	}

	// Build the current model predictions
	buildPredictions( *problem, models, curStep );

	// The vectors in the regression value are partial derivatives of the loss function
	// The tree built for this problem will decrease the loss function value
	lossFunction.CalcGradientAndHessian( predicts, answers, gradients, hessians );

	// Add the vector weights and calculate the total
	CArray<double> gradientsSum;
	gradientsSum.Add( 0, gradients.Size() );
	CArray<double> hessiansSum;
	hessiansSum.Add( 0, gradients.Size() );
	CArray<double> weights;
	weights.SetSize( usedVectors.Size() );

	double weightsSum = 0;
	for( int i = 0; i < usedVectors.Size(); i++ ) {
		weights[i] = problem->GetVectorWeight( usedVectors[i] );
		weightsSum += weights[i];
	}

	for( int i = 0; i < gradients.Size(); i++ ) {
		for( int j = 0; j < usedVectors.Size(); j++ ) {
			gradients[i][j] = gradients[i][j] * weights[j];
			gradientsSum[i] += gradients[i][j];
			hessians[i][j] = hessians[i][j] * weights[j];
			hessiansSum[i] += hessians[i][j];
		}
	}

	if( params.Subfeature != 1.0 || params.Subsample != 1.0 ) {
		// The sub-problem data has changed, reload it
		if( fullProblem != nullptr ) {
			fullProblem->Update();
		}
	}

	if( fullMultiClassTreeBuilder != nullptr || fastHistMultiClassTreeBuilder != nullptr ) {
		if( fullMultiClassTreeBuilder != nullptr ) {
			curModels.Add( fullMultiClassTreeBuilder->Build( *fullProblem,
				gradients, gradientsSum, hessians, hessiansSum, weights, weightsSum ).Ptr() );
		} else {
			curModels.Add( fastHistMultiClassTreeBuilder->Build( *fastHistProblem, gradients, hessians, weights ).Ptr() );
		}
	} else {
		for( int i = 0; i < gradients.Size(); i++ ) {
			if( logStream != nullptr ) {
				*logStream << "GradientSum = " << gradientsSum[i]
					<< " HessianSum = " << hessiansSum[i]
					<< "\n";
			}
			CPtr<IRegressionTreeNode> model;
			if( fullSingleClassTreeBuilder != nullptr ) {
				model = fullSingleClassTreeBuilder->Build( *fullProblem,
					gradients[i], gradientsSum[i],
					hessians[i], hessiansSum[i],
					weights, weightsSum );
			} else {
				model = fastHistSingleClassTreeBuilder->Build( *fastHistProblem, gradients[i], hessians[i], weights );
			}
			curModels.Add( model );
		}
	}
}

// Builds the ensemble predictions for a set of vectors
void CGradientBoost::buildPredictions( const IMultivariateRegressionProblem& problem, const CArray<CGradientBoostEnsemble>& models, int curStep )
{
	CFloatMatrixDesc matrix = problem.GetMatrix();
	NeoAssert( matrix.Height == problem.GetVectorCount() );
	NeoAssert( matrix.Width == problem.GetFeatureCount() );

	CArray<CFastArray<double, 1>> predictions;
	predictions.SetSize( params.ThreadCount );
	for( int i = 0; i < predictions.Size(); i++ ) {
		predictions[i].SetSize( problem.GetValueSize() );
	}

	NEOML_OMP_NUM_THREADS( params.ThreadCount )
	{
		int index = 0;
		int count = 0;
		int threadNum = OmpGetThreadNum();
		if( OmpGetTaskIndexAndCount( usedVectors.Size(), index, count ) ) {
			for( int i = 0; i < count; i++ ) {
				const int usedVector = usedVectors[index];
				const CFloatVector value = problem.GetValue( usedVectors[index] );
				CFloatVectorDesc vector;
				matrix.GetRow( usedVector, vector );

				if( isMultiTreesModel() ) {
					CGradientBoostModel::PredictRaw( models[0], predictCache[0][usedVector].Step,
						params.LearningRate, vector, predictions[threadNum] );
				} else {
					CFastArray<double, 1> pred;
					pred.SetSize(1);
					for( int j = 0; j < problem.GetValueSize(); j++ ) {
						 CGradientBoostModel::PredictRaw( models[j], predictCache[j][usedVector].Step,
							 params.LearningRate, vector, pred );
						 predictions[threadNum][j] = pred[0];
					}
				}

				for( int j = 0; j < problem.GetValueSize(); j++ ) {
					predictCache[j][usedVector].Value += predictions[threadNum][j];
					predictCache[j][usedVector].Step = curStep;
					predicts[j][index] =  predictCache[j][usedVector].Value;
					answers[j][index] = value[j];
				}
				index++;
			}
		}
	}
}

// Fills the prediction cache with the values of the full problem
void CGradientBoost::buildFullPredictions( const IMultivariateRegressionProblem& problem, const CArray<CGradientBoostEnsemble>& models )
{
	CFloatMatrixDesc matrix = problem.GetMatrix();
	NeoAssert( matrix.Height == problem.GetVectorCount() );
	NeoAssert( matrix.Width == problem.GetFeatureCount() );

	for( int i = 0; i < predicts.Size(); i++ ) {
		predicts[i].SetSize( problem.GetVectorCount() );
		answers[i].SetSize( problem.GetVectorCount());
	}
	CArray<CFastArray<double, 1>> predictions;
	predictions.SetSize( params.ThreadCount );
	for( int i = 0; i < predictions.Size(); i++ ) {
		predictions[i].SetSize( problem.GetValueSize() );
	}

	int step = models[0].Size();
	NEOML_OMP_NUM_THREADS( params.ThreadCount )
	{
		int index = 0;
		int count = 0;
		int threadNum = OmpGetThreadNum();
		if( OmpGetTaskIndexAndCount( problem.GetVectorCount(), index, count ) ) {
			for( int i = 0; i < count; i++ ) {
				const CFloatVector value = problem.GetValue( index );
				CFloatVectorDesc vector;
				matrix.GetRow( index, vector );

				if( isMultiTreesModel() ){
					CGradientBoostModel::PredictRaw( models[0], predictCache[0][index].Step,
						params.LearningRate, vector, predictions[threadNum] );
				} else {
					CFastArray<double, 1> pred;
					pred.SetSize(1);
					for( int j = 0; j < problem.GetValueSize(); j++ ){
						CGradientBoostModel::PredictRaw( models[j], predictCache[j][index].Step,
							params.LearningRate, vector, pred );
						predictions[threadNum][j] = pred[0];
					}
				}

				for( int j = 0; j < problem.GetValueSize(); j++ ) {
					predictCache[j][index].Value += predictions[threadNum][j];
					predictCache[j][index].Step = step;
					predicts[j][index] = predictCache[j][index].Value;
					answers[j][index] = value[j];
				}
				index++;
			}
		}
	}
}

// Creates model represetation requested in params.
CPtr<IObject> CGradientBoost::createOutputRepresentation(
	CArray<CGradientBoostEnsemble>& models, int predictionSize )
{
	CPtr<CGradientBoostModel> linked = FINE_DEBUG_NEW CGradientBoostModel(
		models, predictionSize, params.LearningRate, params.LossFunction );

	switch( params.Representation ) {
		case GBMR_Linked:
			return linked.Ptr();
		case GBMR_Compact:
			linked->ConvertToCompact();
			return linked.Ptr();
		case GBMR_QuickScorer:
			return CGradientBoostQuickScorer().Build( *linked ).Ptr();
		default:
			NeoAssert( false );
			return 0;
	}
}

void CGradientBoost::prepareProblem( const IProblem& _problem )
{
	if( baseProblem == 0 ) {
		CPtr<const IMultivariateRegressionProblem> multivariate;
		if( _problem.GetClassCount() == 2 ) {
			multivariate = FINE_DEBUG_NEW CMultivariateRegressionOverBinaryClassification( &_problem );
		} else {
			multivariate = FINE_DEBUG_NEW CMultivariateRegressionOverClassification( &_problem );
		}

		baseProblem = FINE_DEBUG_NEW CMultivariateRegressionProblemNotNullWeightsView( multivariate );
		initialize();
	}
}

void CGradientBoost::prepareProblem( const IRegressionProblem& _problem )
{
	if( baseProblem == 0 ) {
		CPtr<const IMultivariateRegressionProblem> multivariate =
			FINE_DEBUG_NEW CMultivariateRegressionOverUnivariate( &_problem );
		baseProblem = FINE_DEBUG_NEW CMultivariateRegressionProblemNotNullWeightsView( multivariate );
		initialize();
	}
}

void CGradientBoost::prepareProblem( const IMultivariateRegressionProblem& _problem )
{
	if( baseProblem == 0 ) {
		baseProblem = FINE_DEBUG_NEW CMultivariateRegressionProblemNotNullWeightsView( &_problem );
		initialize();
	}
}

bool CGradientBoost::TrainStep( const IProblem& _problem )
{
	prepareProblem( _problem );
	return trainStep();
}

bool CGradientBoost::TrainStep( const IRegressionProblem& _problem )
{
	prepareProblem( _problem );
	return trainStep();
}

bool CGradientBoost::TrainStep( const IMultivariateRegressionProblem& _problem )
{
	prepareProblem( _problem );
	return trainStep();
}

bool CGradientBoost::trainStep()
{
	try {
		if( logStream != nullptr ) {
			*logStream << "\nBoost iteration " << models[0].Size() << ":\n";
		}

		// Gradient boosting step
		CObjectArray<IRegressionTreeNode> curIterationModels; // a new model for multi-class classification
		executeStep( *lossFunction, baseProblem, curIterationModels );

		for( int j = 0; j < curIterationModels.Size(); j++ ) {
			models[j].Add( curIterationModels[j] );
		}
	} catch( ... ) {
		destroyTreeBuilder(); // return to the initial state
		throw;
	}

	return models[0].Size() >= params.IterationsCount;
}

void CGradientBoost::Serialize( CArchive& archive )
{
	if( archive.IsStoring() ) {
		archive << models.Size();
		if( models.Size() > 0 ) {
			archive << models[0].Size();
			for( int i = 0; i < models.Size(); i++ ) {
				CGradientBoostEnsemble& ensemble = models[i];
				for( int j = 0; j < ensemble.Size(); j++ ) {
					ensemble[j]->Serialize( archive );
				}
			}
		}
		predictCache.Serialize( archive );
	} else {
		int ensemblesCount;
		archive >> ensemblesCount;
		if( ensemblesCount > 0 ) {
			models.SetSize( ensemblesCount );
			int iterationsCount;
			archive >> iterationsCount;
			if( iterationsCount > 0 ) {
				for( int i = 0; i < models.Size(); i++ ) {
					models[i].SetSize( iterationsCount );
					for( int j = 0; j < iterationsCount; j++ ) {
						models[i][j] = CreateModel<IRegressionTreeNode>( "FmlRegressionTreeModel" );
						models[i][j]->Serialize( archive );
					}
				}
			}
		}
		predictCache.Serialize( archive );
	}
}

template<typename T>
CPtr<T> CGradientBoost::getModel()
{
	// Calculate the last loss values
	buildFullPredictions( *baseProblem, models );
	loss = lossFunction->CalcLossMean( predicts, answers );

	int predictionSize = isMultiTreesModel() ? baseProblem->GetValueSize() : 1;
	destroyTreeBuilder();
	predictCache.DeleteAll();

	return CheckCast<T>( createOutputRepresentation( models, predictionSize ) );
}

CPtr<IModel> CGradientBoost::GetClassificationModel( const IProblem& _problem )
{
	prepareProblem( _problem );
	return getModel<IModel>();
}

CPtr<IRegressionModel> CGradientBoost::GetRegressionModel( const IRegressionProblem& _problem )
{
	prepareProblem( _problem );
	return getModel<IRegressionModel>();
}

CPtr<IMultivariateRegressionModel> CGradientBoost::GetMultivariateRegressionModel( const IMultivariateRegressionProblem& _problem )
{
	prepareProblem( _problem );
	return getModel<IMultivariateRegressionModel>();
}

} // namespace NeoML
