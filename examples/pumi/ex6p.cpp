//                       PUMI Example 6 - Parallel Version
//
//
// Sample runs:  mpirun -np 8 ./pumi_ma_ex6p 
//
// Description:  This is a version of Example 1 with a simple adaptive mesh
//               refinement loop. The problem being solved is again the Laplace
//               equation -Delta u = 1 with homogeneous Dirichlet boundary
//               conditions. The problem is solved on a sequence of meshes which
//               are adapted in a conforming (tetrahedrons) manner according
//               to a simple SPR ZZ error estimator.
//
//               This example also performs a "uniform" refinement, similar to
//               MFEM examples, for coarse meshes. However, the refinement is
//               performed using the PUMi Api's.
// 
//               A new switch "-ar" is added to modify the "adapt_ratio" which 
//               is the fraction of allowable error that scales the output                                            
//               size field of the error estimator. 


#include "mfem.hpp"
#include <fstream>
#include <iostream>

#ifdef MFEM_USE_SIMMETRIX
#include <SimUtil.h>
#include <gmi_sim.h>
#endif
#include <apfMDS.h>
#include <gmi_null.h>
#include <PCU.h>
#include <spr.h>
#include <apfConvert.h>
#include <gmi_mesh.h>
#include <crv.h>


using namespace std;
using namespace mfem;


int main(int argc, char *argv[])
{
   // 1. Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 2. Parse command-line options.
   const char *mesh_file = "../../data/pumi/parallel/Kova/Kova100k_8.smb";
#ifdef MFEM_USE_SIMMETRIX
   const char *model_file = "../../data/pumi/geom/Kova.x_t";
   const char *smd_file = NULL;
#else
   const char *model_file = "../../data/pumi/geom/Kova.dmg";
#endif
   int order = 1;
   bool static_cond = false;
   bool visualization = 1;
   int geom_order = 1;
   double adapt_ratio = 0.05;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree) or -1 for"
                  " isoparametric space.");
   args.AddOption(&static_cond, "-sc", "--static-condensation", "-no-sc",
                  "--no-static-condensation", "Enable static condensation.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&model_file, "-p", "--model",
                  "parasolid or .dmg model to use.");  
   args.AddOption(&smd_file, "-sm", "--smd_model",
                  "smd model file to use.");   
   args.AddOption(&geom_order, "-go", "--geometry_order",
                  "Geometric order of the model");
   args.AddOption(&adapt_ratio, "-ar", "--adapt_ratio",
                  "adaptation factor used in MeshAdapt");   

   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }

   //3. Read the SCOREC Mesh
   PCU_Comm_Init();
#ifdef MFEM_USE_SIMMETRIX
   Sim_readLicenseFile(0);
   gmi_sim_start();
   gmi_register_sim();
#endif
   gmi_register_mesh();

   apf::Mesh2* pumi_mesh;
   if (smd_file){
       gmi_model *mixed_model = gmi_sim_load(model_file, smd_file);
       pumi_mesh = apf::loadMdsMesh(mixed_model, mesh_file);
   }
   else
        pumi_mesh = apf::loadMdsMesh(model_file, mesh_file);

   //4. Increase the geometry order and refine the mesh if necessary.
   //   Parallel uniform refinement is performed if the total numebr of
   //   elements is less than 10000.
   int dim = pumi_mesh->getDimension();
   int nEle = pumi_mesh->count(dim);
   int ref_levels = (int)floor(log(100000./nEle)/log(2.)/dim);

   if (geom_order > 1)
   {
      crv::BezierCurver bc(pumi_mesh, geom_order, 2);
      bc.run();
   }

   // Perform Uniform refinement
   if (myid == 1)
     std::cout << " ref level : " <<     ref_levels << std::endl; 
   
   if (ref_levels > 1)
   {
      ma::Input* uniInput = ma::configureUniformRefine(pumi_mesh, ref_levels);

      if ( geom_order > 1)
      {
         crv::adapt(uniInput);
      }
      else
      {
         ma::adapt(uniInput);
      }
   }

   pumi_mesh->verify();

   // 5. Create the parallel MFEM mesh object from the parallel PUMI mesh.
   //    We can handle triangular and tetrahedral meshes. Note that the
   //    mesh resolution is performed on the PUMI mesh.
   ParMesh *pmesh = new ParPumiMesh(MPI_COMM_WORLD, pumi_mesh);


   // 6. Define a parallel finite element space on the parallel mesh. Here we
   //    use continuous Lagrange finite elements of the specified order. If
   //    order < 1, we instead use an isoparametric/isogeometric space.
   FiniteElementCollection *fec;
   if (order > 0)
   {
      fec = new H1_FECollection(order, dim);
   }
   else if (pmesh->GetNodes())
   {
      fec = pmesh->GetNodes()->OwnFEC();
      if (myid == 1)
      {
         cout << "Using isoparametric FEs: " << fec->Name() << endl;
      }
   }
   else
   {
      fec = new H1_FECollection(order = 1, dim);
   }
   ParFiniteElementSpace *fespace = new ParFiniteElementSpace(pmesh, fec);
   HYPRE_Int size = fespace->GlobalTrueVSize();
   if (myid == 1)
   {
      cout << "Number of finite element unknowns: " << size << endl;
   }

   // 7. Determine the list of true (i.e. parallel conforming) essential
   //    boundary dofs. In this example, the boundary conditions are defined
   //    by marking all the boundary attributes from the mesh as essential
   //    (Dirichlet) and converting them to a list of true dofs.


   // 8. Set up the parallel linear form b(.) which corresponds to the
   //    right-hand side of the FEM linear system, which in this case is
   //    (1,phi_i) where phi_i are the basis functions in fespace.
   ParLinearForm *b = new ParLinearForm(fespace);
   ConstantCoefficient one(1.0);
   b->AddDomainIntegrator(new DomainLFIntegrator(one));
   

   // 9. Define the solution vector x as a parallel finite element grid function
   //    corresponding to fespace. Initialize x with initial guess of zero,
   //    which satisfies the boundary conditions.
   ParGridFunction x(fespace);
   x = 0.0;

   // 10. Set up the parallel bilinear form a(.,.) on the finite element space
   //     corresponding to the Laplacian operator -Delta, by adding the Diffusion
   //     domain integrator.
   ParBilinearForm *a = new ParBilinearForm(fespace);
   a->AddDomainIntegrator(new DiffusionIntegrator(one));

   // 11. Assemble the parallel bilinear form and the corresponding linear
   //     system, applying any necessary transformations such as: parallel
   //     assembly, eliminating boundary conditions, applying conforming
   //     constraints for non-conforming AMR, static condensation, etc.
   if (static_cond) { a->EnableStaticCondensation(); }
   
   // 12. The main AMR loop. In each iteration we solve the problem on the
   //     current mesh, visualize the solution, and adapt the mesh.
   //     
   apf::Field* Tmag_field = 0;
   apf::Field* temp_field = 0; 
   apf::Field* ipfield = 0;
   apf::Field* sizefield = 0;
   int max_iter = 3;

   for (int Itr = 0; Itr < max_iter; Itr++)
   {    
        HYPRE_Int global_dofs = fespace->GlobalTrueVSize();
        if (myid == 1)
        {
         cout << "\nAMR iteration " << Itr << endl;
         cout << "Number of unknowns: " << global_dofs << endl;
        }        
        
        //Assemble 
        a->Assemble();
        b->Assemble();
        
        //Essentail boundary condition
        Array<int> ess_tdof_list;
        if (pmesh->bdr_attributes.Size())
        {
            Array<int> ess_bdr(pmesh->bdr_attributes.Max());
            ess_bdr = 1;
            fespace->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
        }      
        
        //Form linear system
        HypreParMatrix A;
        Vector B, X;
        const int copy_interior = 1;
        a->FormLinearSystem(ess_tdof_list, x, *b, A, X, B, copy_interior);        

        // 12. Define and apply a parallel PCG solver for AX=B with the BoomerAMG
        //     preconditioner from hypre.
        HypreBoomerAMG amg;
        amg.SetPrintLevel(0);
        CGSolver pcg(A.GetComm());
        pcg.SetPreconditioner(amg);
        pcg.SetOperator(A);
        pcg.SetRelTol(1e-6);
        pcg.SetMaxIter(200);
        pcg.SetPrintLevel(3); // print the first and the last iterations only
        pcg.Mult(B, X);  
      
        // 13. Recover the parallel grid function corresponding to X. This is the
        //     local finite element solution on each processor.
        a->RecoverFEMSolution(X, *b, x);
        
        // 16. Save in parallel the displaced mesh and the inverted solution (which
        //     gives the backward displacements to the original grid). This output
        //     can be viewed later using GLVis: "glvis -np <np> -m mesh -g sol".
        {
           ostringstream mesh_name, sol_name;
           mesh_name << "mesh." << setfill('0') << setw(6) << myid;
           sol_name << "sol." << setfill('0') << setw(6) << myid;

           ofstream mesh_ofs(mesh_name.str().c_str());
           mesh_ofs.precision(8);
           pmesh->Print(mesh_ofs);

           ofstream sol_ofs(sol_name.str().c_str());
           sol_ofs.precision(8);
           x.Save(sol_ofs);
        }

        // 17. Send the above data by socket to a GLVis server.  Use the "n" and "b"
        //     keys in GLVis to visualize the displacements.
        if (visualization)
        {
           char vishost[] = "localhost";
           int  visport   = 19916;
           socketstream sol_sock(vishost, visport);
           sol_sock << "parallel " << num_procs << " " << myid << "\n";
           sol_sock.precision(8);
           sol_sock << "solution\n" << *pmesh << x << flush;
        }       

        // 18. Field transfer. Scalar solution field and magnitude field for 
        //     error estimation are created the pumi mesh. 
        if (order > geom_order)
        {
            Tmag_field = apf::createField(pumi_mesh, "field_mag",
                    apf::SCALAR, apf::getLagrange(order));
            temp_field = apf::createField(pumi_mesh, "T_field",
                    apf::SCALAR, apf::getLagrange(order));
        }
        else
        {
            Tmag_field = apf::createFieldOn(pumi_mesh, "field_mag",apf::SCALAR);
            temp_field = apf::createFieldOn(pumi_mesh, "T_field", apf::SCALAR);  
        }

        ParPumiMesh* pPPmesh = dynamic_cast<ParPumiMesh*>(pmesh);
        pPPmesh->FieldMFEMtoPUMI(pumi_mesh, &x, temp_field, Tmag_field);

        ipfield= spr::getGradIPField(Tmag_field, "MFEM_gradip", 2);
        sizefield = spr::getSPRSizeField(ipfield, adapt_ratio);

        apf::destroyField(Tmag_field);
        apf::destroyField(ipfield);
        apf::destroyNumbering(pumi_mesh->findNumbering("LocalVertexNumbering"));
        
        // 19. Perform MesAdapt
        ma::Input* erinput = ma::configure(pumi_mesh, sizefield);
        erinput->shouldFixShape = true;
        erinput->maximumIterations = 2;
        if ( geom_order > 1)
             crv::adapt(erinput);
        else
             ma::adapt(erinput);   

        ParMesh* Adapmesh = new ParPumiMesh(MPI_COMM_WORLD, pumi_mesh);
        pPPmesh->UpdateMesh(Adapmesh);
        delete Adapmesh;
        
        // 20. Update the FiniteElementSpace, Gridfunction, ad bilinear form
        fespace->Update();
        x.Update();
        x = 0.0;

        pPPmesh->FieldPUMItoMFEM(pumi_mesh, temp_field, &x);
        a->Update();
        b->Update();
        
        //Destroy fields 
        apf::destroyField(temp_field);
        apf::destroyField(sizefield);         
   }
   
   // 16. Free the used memory.
   delete a;
   delete b;
   delete fespace;
   if (order > 0) { delete fec; }
   delete pmesh;   
   
   pumi_mesh->destroyNative();
   apf::destroyMesh(pumi_mesh);
   PCU_Comm_Free();

#ifdef MFEM_USE_SIMMETRIX
   gmi_sim_stop();
   Sim_unregisterAllKeys();
#endif

   MPI_Finalize();

   return 0;
}