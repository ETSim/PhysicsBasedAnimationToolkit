target_sources(PhysicsBasedAnimationToolkit_PhysicsBasedAnimationToolkit
    PRIVATE
    FILE_SET implementation
    FILES
    "Buffer.cuh"
    "Common.h"
    "Cuda.cuh"
    "Eigen.cuh"
    "SynchronizedList.cuh"
    "Var.cuh"
)

target_sources(PhysicsBasedAnimationToolkit_PhysicsBasedAnimationToolkit
    PRIVATE
    "Buffer.cu"
    "Cuda.cu"
    "Eigen.cu"
    "SynchronizedList.cu"
    "Var.cu"
)