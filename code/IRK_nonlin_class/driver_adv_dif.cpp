#include <iostream>
#include <fstream>
#include "mfem.hpp"
#include "FDSpaceDisc.hpp"
#include "IRK.hpp"

using namespace mfem;
using namespace std;

#define PI 3.14159265358979323846

// mpirun -np 4 ./driver_adv_dif -f 1 -d 1 -o 10 -ex 1 -mx 0.1 -my 0.1 -l 6 -dt -5 -t 110 -save 2 -tf 1 -np 2 -nmaxit 20 -kp 2 -nrtol 1e-8 -nktol 1e-12

// TODO: Segfault associated w/ freeing HypreBoomerAMG... Currently not being
// deleted... See associated `TODO` comment...

// Sample runs:
//      *** Solve 2D in space problem w/ 4th-order discretizations in space & time ***
//      --- linear problem ---
//      mpirun -np 4 ./driver -f 0 -gf 0 -d 2 -t 14 -o 4 -ex 1 -mx 0.075 -my 0.075 -dt -2 -l 5 
//      --- linear problem solved with Newton's method ---
//      mpirun -np 4 ./driver -f 0 -gf 1 -d 2 -t 14 -o 4 -ex 1 -mx 0.075 -my 0.075 -dt -2 -l 5 
//      --- nonlinear problem ---
//      mpirun -np 4 ./driver -f 1 -d 2 -t 14 -o 4 -ex 1 -mx 0.075 -my 0.075 -dt -2 -l 5 
// 
// 
// Solve scalar advection-diffusion equation,
//     u_t + alpha.grad(f(u)) = div( mu \odot grad(u) ) + s(x,t),
// for constant vectors alpha, and mu.
// 
// An initial condition is specified, and periodic spatial boundaries are used.
// 
// Options for advection flux f are,
//     f(u) = u   -- linear advection,                      [pass `-f 0`]
//     f(u) = u^2 -- nonlinear advection (Burgers' like).   [pass `-f 1`]
// 
// Implemented in 1 and 2 spatial dimensions.               
// 
// Test problems with manufactured solutions are implemented to facillitate 
// convergence studies.                                     [pass `-ex 1` (OR `-ex 2` for f(u)==u for a different example) to report error]
//
// TIME DISCRETIZATION: Both explicit and implicit RK options are available.
//
// SPATIAL DISCRETIZATION: Arbitrarily high-order FD is are available. 
// NOTE: No sophisticated treatment is provided for the discretization of 
// f(u)=u^2/2, which means that for small diffusivities mu, the discrete solution
// will become unstable due to the development of sharp gradients (however, the
// development of such gradients is dependent on what s(x,t) is, of course).


/* Output dictionary entries via an ofstream */
class OutDict : public std::ofstream {
public:
    template <class T>
    inline void Print(string id, T entry) {*this << id << " " << entry << "\n";};
};

// Problem parameters global functions depend on
Vector alpha, mu;
int dim, problem, fluxID;
bool GENERAL_FLUX;

enum Linearity { LINEAR = 0, NONLINEAR = 1 };
class BEOper;
class JacPrec;

/* Functions definining some parts of PDE */
double InitialCondition(const Vector &x);
double Source(const Vector &x, double t);
double PDESolution(const Vector &x, double t);


struct AMG_params {
    bool use_AIR = !true; 
    double distance = 1.5;
    string prerelax = "";
    string postrelax = "FFC";
    double strength_tolC = 0.1;
    double strength_tolR = 0.01;
    double filter_tolR = 0.0;
    int interp_type = 100;
    int relax_type = 0;
    double filterA_tol = 0.e-4;
    int coarsening = 6;
};

/** Provides the time-dependent RHS of the ODEs after spatially discretizing the 
    PDE,
        du/dt = L(u,t) == - A(u) + D*u + s(t).
    where:
        A: The advection discretization,
        D: The diffusion discretization,
        s: The solution-independent source term discretization. */
class AdvDif : public IRKOperator
{
private:    
    Linearity op_type;          // (assumed) Linearity of flux function
    
    int dim;                    // Spatial dimension
    FDMesh &Mesh;               // Mesh on which the PDE is discretized
    
    Vector alpha;               // Advection coefficients
    Vector mu;                  // Diffusion coefficients
    FDLinearOp * D;             // Diffusion operator
    FDNonlinearOp * A;          // Nonlinear advection operator
        
    mutable Vector source, temp;// Solution independent source term & auxillary vector

    // Preconditioners for systems of the form B = gamma*I-dt*Jacobian
    Array<HypreBoomerAMG *> B_prec; // 
    Array<HypreParMatrix *> B_mats; // Seem's like it's only safe to free matrix once preconditioner is finished with it, so need to save these...
    AMG_params AMG;                 // AMG solver params
    mutable bool JacobianUpdated; // Jacobian updated since any calls to SetSystem()
    int B_index;
    
    // Compatible identity matrix
    HypreParMatrix * identity;
    
    // dL/du
    mutable HypreParMatrix * Jacobian;
    
    /// Set solution independent source term at current time.
    inline void SetSource() const { Mesh.EvalFunction(&Source, GetTime(), source); };
    
public:
    
    AdvDif(FDMesh &Mesh_, int fluxID, Vector alpha_, Vector mu_, 
            int order, FDBias advection_bias);
    ~AdvDif();
    
    /// Allocate memory to and assign initial condition.
    inline void GetU0(Vector * &u) const { Mesh.EvalFunction(&InitialCondition, u); };
    
    /// Compute the right-hand side of the ODE system.
    void Mult(const Vector &u, Vector &du_dt) const;
    
    /// Compute the right-hand side of the ODE system. (Some applications require this operator too)
    void ExplicitMult(const Vector &u, Vector &du_dt) const { Mult(u, du_dt); };
    
    /** Gradient of L(u, t) w.r.t u evaluated at x */
    HypreParMatrix &GetExplicitGradient(const Vector &x) const;
    
    
    /// Get error w.r.t. exact PDE solution (if available)
    bool GetError(int save, const char * out, double t, const Vector &u, 
                    double &eL1, double &eL2, double &eLinf);
    

    /** Precondition B*x=y <==> (\gamma*I - dt*L')*x=y */
    inline void ImplicitPrec(const Vector &x, Vector &y) const {
        MFEM_ASSERT(B_prec[B_index], 
            "AdvDif::ImplicitPrec() Must first set system! See SetSystem()");
        B_prec[B_index]->Mult(x, y);
    }
    
    
    // Function to ensure that ImplicitPrec preconditions (\gamma*M - dt*L') OR (\gamma*I - dt*L')
    // with gamma and dt as passed to this function.
    //      + index -> index of system, [0,s_eff)
    //      + type -> eigenvalue type, 1 = real, 2 = complex pair
    //      + t -> time.
    // These additional parameters are to provide ways to track when
    // (\gamma*M - dt*L') _OR_ (\gamma*I - dt*L') must be reconstructed or not to minimize setup.
    // NOTE: Must use the Operator that a reference is returned to via calling
    // GetExplicitGradient().
    void SetSystem(int index, double dt, double gamma, int type);
    
    /// Set solver parameters fot implicit time-stepping; MUST be called before InitSolvers()
    inline void SetAMGParams(AMG_params params) { AMG = params; };
};


/** Gradient of L(u, t) w.r.t u evaluated at x, dL/du(x) = -dA/du(x) + D */
// TODO:  deal with memory leaks here... who owns the jacobian, and who owns
// the components that are added
HypreParMatrix &AdvDif::GetExplicitGradient(const Vector &x) const {
    if (Jacobian) delete Jacobian;

    JacobianUpdated = true;

    if (A && D) {
        Jacobian = HypreParMatrixAdd(-1., A->GetGradient(x), 1., D->Get()); 
    } else if (A) {
        Jacobian = &(A->GetGradient(x));
        *Jacobian *= -1.;
    } else if (D) {
        Jacobian = &(D->Get());
    }
    return *Jacobian;
}

void AdvDif::SetSystem(int index, double dt, double gamma, int type) {
    MFEM_ASSERT(Jacobian, "AdvDif::SetSystem() Jacobian not yet set!");
    
    B_index = index;
    
    // If Jacobian has been updated since last call to this function, delete all
    // existing preconditioners used existing Jacobian.
    if (JacobianUpdated) {
        for (int i = 0; i < B_prec.Size(); i++) {
            delete B_prec[i]; 
            B_prec[i] = NULL;
            delete B_mats[i];
            B_mats[i] = NULL;
        }
        JacobianUpdated = false;
    }
    
    // Make space for new preconditioner to be built 
    if (index >= B_prec.Size()) {
        B_prec.Append(NULL);
        B_mats.Append(NULL);
    }
    
    // Build a new preconditioner 
    if (!B_prec[index]) {
        
        // Assemble identity matrix 
        if (!identity) identity = (A) ? A->GetHypreParIdentityMatrix() : D->GetHypreParIdentityMatrix();
        
        // B = gamma*I - dt*Jacobian
        HypreParMatrix * B = HypreParMatrixAdd(-dt, *Jacobian, gamma, *identity); 
        
        /* Build AMG preconditioner for B */
        HypreBoomerAMG * amg_solver = new HypreBoomerAMG(*B);
        
        amg_solver->SetPrintLevel(0); 
        amg_solver->SetMaxIter(1); 
        amg_solver->SetTol(0.0);
        amg_solver->SetMaxLevels(50); 
        //amg_solver->iterative_mode = false;
        if (AMG.use_AIR) {                        
            amg_solver->SetLAIROptions(AMG.distance, 
                                        AMG.prerelax, AMG.postrelax,
                                        AMG.strength_tolC, AMG.strength_tolR, 
                                        AMG.filter_tolR, AMG.interp_type, 
                                        AMG.relax_type, AMG.filterA_tol,
                                        AMG.coarsening);                                       
        } else {
            amg_solver->SetInterpolation(0);
            amg_solver->SetCoarsening(AMG.coarsening);
            amg_solver->SetAggressiveCoarsening(1);
        } 
        
        B_prec[index] = amg_solver;
        B_mats[index] = B;
    }
}




/* NOTES:
 - Cannot simultaneously specify dt, nt, and tf. Values of dt and tf take precidense 
 over nt. By default, however, nt and tf are specified and dt is determined from 
 them accordingly. */
int main(int argc, char *argv[])
{
    // Initialize parallel
    int myid, numProcess;
    MPI_Status status;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);
    MPI_Comm_size(MPI_COMM_WORLD, &numProcess);


    /* ------------------------------------------------------ */
    /* --- Set default values for command-line parameters --- */
    /* ------------------------------------------------------ */
    /* PDE */
    problem = 1; // Problem ID: responsible for generating source and exact solution
    GENERAL_FLUX = false; // Setting GENERAL_FLUX==TRUE allows for the "nonlinear treatment" of linear problem. 
    int GENERAL_FLUX_temp = 0;
    FDBias advection_bias = CENTRAL;
    int advection_bias_temp = static_cast<int>(advection_bias);
    double ax=1.0, ay=1.0; // Advection coefficients
    double mx=1.0, my=1.0; // Diffusion coefficients
    dim = 1; // Spatial dimension
    
    /* --- Time integration --- */
    int nt     = 10;  // Number of time steps
    double dt  = 0.0; // Time step size. dt < 0 means: dt == |dt|*dx
    double tf  = 2.0; // Final integration time
    int RK_ID  = 1;  // RK scheme (see below)

    /* --- Spatial discretization --- */
    int order     = 2; // Approximation order
    int refLevels = 4; // nx == 2^refLevels in every dimension
    int npx       = -1; // # procs in x-direction
    int npy       = -1; // # procs in y-direction
    
    // Solver parameters
    AMG_params AMG;
    Krylov_params KRYLOV;
    NEWTON_params NEWTON;
    int use_AIR_temp = (int) AMG.use_AIR;
    
    // Output paramaters
    int save         = 0;  // Save solution vector
    const char * out = "data/U"; // Filename of data to be saved...
    
    OptionsParser args(argc, argv);
    args.AddOption(&fluxID, "-f", "--flux-function",
                  "0==Linear==u, 1==Nonlinear==u^2.");
    args.AddOption(&GENERAL_FLUX_temp, "-gf", "--general-flux-function",
                  "1==Ignores linearity of flux if it's linear.");              
    args.AddOption(&problem, "-ex", "--example-problem",
                  "1 (and 2 for linear problem)."); 
    args.AddOption(&ax, "-ax", "--alpha-x",
                  "Advection in x-direction."); 
    args.AddOption(&ay, "-ay", "--alpha-y",
                  "Advection in y-direction."); 
    args.AddOption(&mx, "-mx", "--mu-x",
                  "Diffusion in x-direction."); 
    args.AddOption(&my, "-my", "--mu-y",
                  "Diffusion in y-direction."); 
    args.AddOption(&dim, "-d", "--dim",
                  "Spatial dimension.");              

     /* Time integration */
    args.AddOption(&RK_ID, "-t", "--RK-method",
                  "Time discretization.");
    args.AddOption(&nt, "-nt", "--num-time-steps", "Number of time steps.");    
    args.AddOption(&tf, "-tf", "--tf", "0 <= t <= tf");
    args.AddOption(&dt, "-dt", "--time-step-size", "t_j = j*dt. NOTE: dt<0 means dt==|dt|*dx");

    /* Spatial discretization */
    args.AddOption(&order, "-o", "--order",
                  "FD: Approximation order."); 
    args.AddOption(&advection_bias_temp, "-b", "--bias",
                  "Advection bias: 0==CENTRAL, 1==UPWIND"); 
    args.AddOption(&refLevels, "-l", "--level",
                  "FD: Mesh refinement; 2^refLevels DOFs in each dimension.");
    args.AddOption(&npx, "-px", "--nprocs-in-x",
                  "FD: Number of procs in x-direction.");
    args.AddOption(&npy, "-py", "--nprocs-in-y",
                  "FD: Number of procs in y-direction.");                          
     
    /// --- Solver parameters --- ///   
    /* AMG parameters */
    args.AddOption(&use_AIR_temp, "-air", "--use-air", "0==standard AMG, 1==AIR");
    /* Krylov parameters */
    args.AddOption(&KRYLOV.reltol, "-krtol", "--gmres-rel-tol", "KRYLOV: Relative stopping tolerance");
    args.AddOption(&KRYLOV.abstol, "-katol", "--gmres-abs-tol", "KRYLOV: Absolute stopping tolerance");
    args.AddOption(&KRYLOV.maxiter, "-kmaxit", "--gmres-max-iterations", "KRYLOV: Maximum iterations");
    args.AddOption(&KRYLOV.kdim, "-kdim", "--gmres-dimension", "KRYLOV: Maximum subspace dimension");
    args.AddOption(&KRYLOV.printlevel, "-kp", "--gmres-print", "KRYLOV: Print level");
    /* Newton parameters */
    args.AddOption(&NEWTON.reltol, "-nrtol", "--newton-rel-tol", "Newton: Relative stopping tolerance");
    args.AddOption(&NEWTON.maxiter, "-nmaxit", "--newton-max-iterations", "Newton: Maximum iterations");
    args.AddOption(&NEWTON.printlevel, "-np", "--newton-print", "Newton: Print level");
    
    /* --- Text output of solution etc --- */              
    args.AddOption(&out, "-out", "--out-directory", "Name of output file."); 
    args.AddOption(&save, "-save", "--save-sol-data",
                  "save>0 will save solution info, save>1 also saves solution (and exact solution, if implemented).");              
    args.Parse();
    if (myid == 0) {
        args.PrintOptions(std::cout); 
    }
    // Set final forms of remaing params
    GENERAL_FLUX = (bool) GENERAL_FLUX_temp;
    advection_bias = static_cast<FDBias>(advection_bias_temp);
    AMG.use_AIR = (bool) use_AIR_temp;
    std::vector<int> np = {};
    if (npx != -1) {
        if (dim >= 1) np.push_back(npx);
        if (dim >= 2) np.push_back(npy);
    }
    alpha.SetSize(dim);
    mu.SetSize(dim);
    alpha(0) = ax;
    mu(0) = mx;
    if (dim > 1) {
        alpha(1) = ay;
        mu(1) = my;
    }
    
    
    ////////////////////////////////////////
    // Assemble mesh on which we discretize.
    ////////////////////////////////////////
    FDMesh Mesh(MPI_COMM_WORLD, dim, refLevels, np);
    
    /////////////////////////////////
    // Set up spatial discretization.
    /////////////////////////////////
    AdvDif SpaceDisc(Mesh, fluxID, alpha, mu, order, advection_bias);
    SpaceDisc.SetAMGParams(AMG);
        
    // Get initial condition
    Vector * u = NULL;
    SpaceDisc.GetU0(u);
    
    // Get mesh info
    double dx = Mesh.Get_dx();
    int    nx = Mesh.Get_nx();
    
    
    //////////////
    // Time-march. 
    //////////////
    // dt < 0 means dt == |dt|*dx
    if (dt < 0.0) {
        dt *= -dx;
        nt = ceil(tf/dt);
    // dt not set, choose it according to nt and tf
    } else if (dt == 0.0) {
        dt = tf / (nt - 0);    
    // Adjust nt so that we (at least) step to tf    
    } else {
        nt = ceil(tf/dt);
    }
    
    
    // Build IRK object using spatial discretization 
    IRK MyIRK(&SpaceDisc, static_cast<IRKType>(RK_ID));        

    // Initialize IRK time-stepping solver
    MyIRK.Init(SpaceDisc);
    // Krylov_params KRYLOV2;
    // KRYLOV2.solver = KrylovMethod::GMRES;
    // KRYLOV2.printlevel = KRYLOV.printlevel;
    // MyIRK.SetKrylovParams(KRYLOV, KRYLOV2);
    MyIRK.SetKrylovParams(KRYLOV);
    MyIRK.SetNewtonParams(NEWTON);
    
    
    // Time step 
    double t = 0.0;
    MyIRK.Run(*u, t, dt, tf);
    
    if (fabs(t-tf)>1e-14) {
        if (myid == 0) std::cout << "WARNING: Requested tf of " << tf << " adjusted to " << t << '\n';
    }
    tf = t; // Update final time


    /* ----------------------- */
    /* --- Save solve data --- */
    /* ----------------------- */
    if (save > 0) {
        if (save > 1) {
            ostringstream outt;
            outt << out << "." << myid;
            ofstream sol;
            sol.open(outt.str().c_str());
            u->Print_HYPRE(sol);
            sol.close();
        }
        
        /* Get error against exact PDE solution if available */
        double eL1, eL2, eLinf = 0.0; 
        bool got_error = SpaceDisc.GetError(save, out, tf, *u, eL1, eL2, eLinf);
        
        // Save data to file enabling easier inspection of solution            
        if (myid == 0) {
            OutDict solinfo;
            solinfo.open(out);
            solinfo << scientific;
            
            /* Temporal info */
            solinfo.Print("RK", RK_ID);
            solinfo.Print("dt", dt);
            solinfo.Print("nt", nt);
            solinfo.Print("tf", tf);
            
            /* Spatial info */
            solinfo.Print("dx", dx);
            solinfo.Print("nx", nx);
            solinfo.Print("space_dim", dim);
            solinfo.Print("space_refine", refLevels);
            solinfo.Print("problemID", problem); 
            
            // /* Linear system/solve statistics */
            // solinfo.Print("krtol", reltol);
            // solinfo.Print("katol", abstol);
            // solinfo.Print("kdim", kdim);
            // std::vector<int> avg_iter;
            // std::vector<int> type;
            // std::vector<double> eig_ratio;
            // MyIRK.GetSolveStats(avg_iter, type, eig_ratio);
            // solinfo.Print("nsys", avg_iter.size());
            // for (int system = 0; system < avg_iter.size(); system++) {
            //     solinfo.Print("sys" + to_string(system+1) + "_iters", avg_iter[system]);
            //     solinfo.Print("sys" + to_string(system+1) + "_type", type[system]);
            //     solinfo.Print("sys" + to_string(system+1) + "_eig_ratio", eig_ratio[system]);
            // }
            
            /* Parallel info */
            solinfo.Print("P", numProcess);
            for (int d = 0; d < np.size(); d++) {
                solinfo.Print("np" + to_string(d), np[d]);
            }
            
            /* Error statistics */
            if (got_error) {
                solinfo.Print("eL1", eL1);
                solinfo.Print("eL2", eL2);
                solinfo.Print("eLinf", eLinf);
            }
            
            solinfo.close();
        }
    }
    //delete ode_solver;
    delete u;

    MPI_Finalize();
    return 0;
}




/* Initial condition of PDE */
double InitialCondition(const Vector &x) {
    switch (x.Size()) {
        case 1:
            return pow(sin(PI/2.0*(x(0)-1.0)), 4.0);
        case 2:
            return pow(sin(PI/2.0*(x(0)-1.0)), 4.0) * pow(sin(PI/2.0*(x(1)-1.0)), 4.0);
        default:
            return 0.0;
    }
}


/* Solution-independent source term in PDE */
double Source(const Vector &x, double t) {
    switch (problem) {
        // Source is chosen for manufactured solution
        case 1:
            switch (fluxID) {
                // Linear advection flux
                case 0:
                    switch (x.Size()) {
                        case 1: 
                        {
                            double u = PI*(x(0)-alpha(0)*t);
                            return mu(0)/8.*exp(-mu(0)*t)*(
                                    -3.+4.*(PI*PI-1.)*cos(u)
                                    +(4.*PI*PI-1.)*cos(2*u));
                        }
                        case 2:
                        {
                            double u = 0.5*PI*(x(0)-alpha(0)*t-1.);
                            double v = 0.5*PI*(x(1)-alpha(1)*t-1.);
                            double z = (mu(0)+mu(1))*t;
                            return -0.5*exp(-z)*pow(sin(u)*sin(v), 2.)
                                    *(  mu(0)*(1.+2.*PI*PI+(4.*PI*PI-1.)*cos(2.*u))*pow(sin(v),2.)
                                      + mu(1)*(1.+2.*PI*PI+(4.*PI*PI-1.)*cos(2.*v))*pow(sin(u),2.) );
                        }
                        default:
                            return 0.0;
                    }
                // Nonlinear advection flux
                case 1:
                    switch (x.Size()) {
                        case 1: 
                        {
                            double u = 0.5*PI*(x(0)-alpha(0)*t);
                            double z = mu(0)*t;
                            return 0.5*exp(-2.*z)*(
                                    -4.*PI*alpha(0)*pow(cos(u),6.)*sin(2.*u)
                                    + exp(z)*pow(cos(u),2.)
                                    * (-mu(0)-2.*PI*PI*mu(0)+(4.*PI*PI-1.)*mu(0)*cos(2.*u) 
                                        + 2.*PI*alpha(0)*sin(2.*u)));
                        }
                        case 2:
                        {
                            double u = 0.5*PI*(x(0)-alpha(0)*t-1.);
                            double v = 0.5*PI*(x(1)-alpha(1)*t-1.);
                            double z = (mu(0)+mu(1))*t;
                            return -0.5*exp(-2.*z)*pow(sin(u)*sin(v), 2.)
                                    *(  2.*PI*alpha(0)*sin(2.*u)*pow(sin(v), 2.)*( exp(z)-2.*pow(sin(u)*sin(v), 4.) )
                                      + 2.*PI*alpha(1)*sin(2.*v)*pow(sin(u), 2.)*( exp(z)-2.*pow(sin(u)*sin(v), 4.) )
                                    + exp(z) 
                                    *(  mu(0)*(1.+2.*PI*PI+(4.*PI*PI-1.)*cos(2.*u))*pow(sin(v),2.)
                                      + mu(1)*(1.+2.*PI*PI+(4.*PI*PI-1.)*cos(2.*v))*pow(sin(u),2.) ));
                        }
                        default:
                            return 0.0;
                    }
            }
        // Default source is 0
        default:
            return 0.0;
    }
}


/* Manufactured PDE solution */
double PDESolution(const Vector &x, double t)
{    
    switch (problem) {
        // Test problem where the initial condition is propagated with wave-speed alpha and dissipated with diffusivity mu
        case 1:
            switch (x.Size()) {
                case 1:
                    return pow(sin(PI/2.*(x(0)-1-alpha(0)*t)), 4.) * exp(-mu(0)*t);
                case 2:
                    return pow(sin(PI/2.*(x(0)-1-alpha(0)*t)) * sin(PI/2.*(x(1)-1-alpha(1)*t)), 4.) 
                            * exp(-(mu(0)+mu(1))*t);
                default:
                    return 0.0;
            }
            break;
        // 2nd test problem for linear-advection-diffusion that's more realistic (no forcing).
        case 2:
            if (fluxID == 1) cout <<  "PDESolution not implemented for NONLINEAR problem " << problem << "\n";
            switch (x.Size()) {
                case 1:
                {   
                    if (fabs(mu(0)) < 1e-15) mfem_error("PDESolution:: Solution only holds for non-zero diffusivity");
                    double u = PI*(x(0)-alpha(0)*t);
                    return 3./8. 
                            + 1./2.*cos(u)*exp(-mu(0)*PI*PI*t) 
                            + 1./8.*cos(2.*u)*exp(-mu(0)*4.*PI*PI*t);
                }
                case 2:
                {   
                    if (fabs(mu(0)) < 1e-15 || fabs(mu(1)) < 1e-15) mfem_error("PDESolution:: Solution only holds for non-zero diffusivity");
                    double u = PI*(x(0)-alpha(0)*t);
                    double v = PI*(x(1)-alpha(1)*t);
                    double p2t = PI*PI*t;
                    double m0 = mu(0);
                    double m1 = mu(1);
                    return 9./64. + 3./16.*cos(u)*exp(-m0*p2t) + 3./64.*cos(2*u)*exp(-4.*m0*p2t)
                            + cos(v)*(3./16.*exp(-m1*p2t) + 1./4.*cos(u)*exp(-(m0+m1)*p2t) + 1./16.*cos(2*u)*exp(-(4.*m0+m1)*p2t))
                            + cos(2.*v)*(3./64.*exp(-4.*m1*p2t) + 1./16.*cos(u)*exp(-(m0+4.*m1)*p2t) + 1./64.*cos(2*u)*exp(-4.*(m0+m1)*p2t));
                }
                default:
                    return 0.0;
            }
        default:
            cout <<  "PDESolution:: not implemented for problem " << problem << "\n";
            return 0.0;
    }
}


/* f(u) = u; fluxID == 0 */
double LinearFlux(double u) { return u; }; 
double GradientLinearFlux(double u) { return 1.0; };
/* f(u) = u^2; fluxID == 1 */
double NonlinearFlux(double u) { return u*u; }; 
double GradientNonlinearFlux(double u) { return 2.*u; };
// Return pointer to flux function
typedef double (*ScalarFun)(double);
ScalarFun Flux(int fluxID) {
    switch (fluxID) {
        case 0:
            return LinearFlux;
        case 1:
            return NonlinearFlux;
        default:
            return NULL;
    }
}
// Return pointer to gradient of flux function
ScalarFun GradientFlux(int fluxID) {
    switch (fluxID) {
        case 0:
            return GradientLinearFlux;
        case 1:
            return GradientNonlinearFlux;
        default:
            return NULL;
    }
}


AdvDif::AdvDif(FDMesh &Mesh_, int fluxID, Vector alpha_, Vector mu_, 
        int order, FDBias advection_bias) 
    : IRKOperator(Mesh_.GetComm(), Mesh_.GetLocalSize()),
        op_type{LINEAR},
        Mesh{Mesh_},
        alpha(alpha_), mu(mu_),
        dim(Mesh_.m_dim),
        D(NULL), A(NULL),
        B_prec(), B_mats(), B_index{0}, AMG(), 
        Jacobian(NULL), identity(NULL), JacobianUpdated(false),
        source(Mesh_.GetLocalSize()), temp(Mesh_.GetLocalSize())
{
    // Assemble diffusion operator if non-zero
    if (mu.Normlp(infinity()) > 1e-15) {
        D = new FDLinearOp(Mesh, 2, mu, order, CENTRAL);
        D->Assemble();
    }
    
    // Assemble advection operator if non-zero
    if (alpha.Normlp(infinity()) > 1e-15) {
        A = new FDNonlinearOp(Mesh, 1, alpha, Flux(fluxID), GradientFlux(fluxID), 
                                order, advection_bias);
    }
    if (!A && !D) mfem_error("AdvDif::AdvDif() Require at least one non-zero PDE coefficient.");
};


/* Evaluate RHS of ODEs: du_dt = -A(u) + D*u + s(t) */
void AdvDif::Mult(const Vector &u, Vector &du_dt) const
{
    if (D && A) {
        D->Mult(u, du_dt);
        A->Mult(u, source); // Use s as auxillary vector, is reset below anyways
        du_dt.Add(-1., source);
    } else if (D) {
        D->Mult(u, du_dt);
    } else {
        A->Mult(u, du_dt);
        du_dt.Neg();
    }
    // Solution-independent source term
    SetSource();
    du_dt += source;
}


/* Get error against exact PDE solution if available. Also output if num solution is output */
bool AdvDif::GetError(int save, const char * out, double t, const Vector &u, double &eL1, double &eL2, double &eLinf) {
    int myid;
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);
    
    bool soln_implemented = false;
    Vector * u_exact = NULL;
    if (problem == 1 || (problem == 2 && fluxID == 0)) {
        if (myid == 0) std::cout << "PDE solution IS implemented for this problem." << '\n';
        soln_implemented = true;
        Mesh.EvalFunction(&PDESolution, t, u_exact); 
    } else {
        if (myid == 0) std::cout << "PDE solution is NOT implemented for this problem." << '\n';
    }
    
    if (soln_implemented) {
        if (save > 1) {
            ostringstream outt;
            outt << out << "_exact" << "." << myid;
            ofstream sol;
            sol.open(outt.str().c_str());
            u_exact->Print_HYPRE(sol);
            sol.close();
        }
        
        *u_exact -= u; // Error vector
        eL1   = u_exact->Normlp(1);
        eL2   = u_exact->Normlp(2);
        eLinf = u_exact->Normlp(infinity());
        delete u_exact;
        
        // Get global norms from on process norms computed above
        eL1   = GlobalLpNorm(1, eL1, MPI_COMM_WORLD);
        eL2   = GlobalLpNorm(2, eL2, MPI_COMM_WORLD);
        eLinf = GlobalLpNorm(infinity(), eLinf, MPI_COMM_WORLD);
        
        // Scale norms by mesh size
        double dx = Mesh.Get_dx(0);
        eL1 *= dx;
        eL2 *= sqrt(dx);
        int dim = Mesh.Get_dim();
        if (dim > 1) {
            double dy = Mesh.Get_dx(1);
            eL1 *= dy;
            eL2 *= sqrt(dy);
        }
        if (myid == 0) {
            std::cout << "Discrete error measured at final time:" << '\n';
            std::cout << "\teL1=" << eL1 << "\n\teL2=" << eL2 << "\n\teLinf=" << eLinf << '\n';    
        }
    }
    return soln_implemented;
}

AdvDif::~AdvDif()
{
    /* TODO: WHY can't I free amg_solver... What's odd is that I can delete these
    solvers elsewhere in the code...
        *** The MPI_Comm_free() function was called after MPI_FINALIZE was invoked.
        *** This is disallowed by the MPI standard.
    */
    
    
    //for (int i = 0; i < B_prec.Size(); i++) delete B_prec[i];
    
    // I can do this one, but not the one above... what gives?
    for (int i = 0; i < B_mats.Size(); i++) delete B_mats[i]; 
    
    if (identity) delete identity;
    if (D) delete D;
    if (A) delete A;
}