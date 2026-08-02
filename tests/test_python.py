import numpy as np

import meshutil


def main():
    positions = np.array(
        [
            [-1, -1, -1],
            [1, -1, -1],
            [1, 1, -1],
            [-1, 1, -1],
            [-1, -1, 1],
            [1, -1, 1],
            [1, 1, 1],
            [-1, 1, 1],
        ],
        dtype=np.float32,
    )
    triangles = np.array(
        [
            [0, 2, 1],
            [0, 3, 2],
            [4, 5, 6],
            [4, 6, 7],
            [0, 1, 5],
            [0, 5, 4],
            [1, 2, 6],
            [1, 6, 5],
            [2, 3, 7],
            [2, 7, 6],
            [3, 0, 4],
            [3, 4, 7],
        ],
        dtype=np.uint32,
    )

    output_positions, output_triangles, stats = meshutil.simplify(
        positions, triangles, 6
    )
    assert output_positions.dtype == np.float32
    assert output_triangles.dtype == np.uint32
    assert output_positions.shape[1] == 3
    assert output_triangles.shape[1] == 3
    assert output_triangles.shape[0] <= 6
    assert stats["input_faces"] == 12
    assert stats["target_reached"]

    _, low_faces, low_stats = meshutil.simplify(
        positions, triangles, 6, memory_mode="low"
    )
    assert low_faces.shape[0] <= 6
    assert low_stats["target_reached"]

    _, partition_faces, partition_stats = meshutil.simplify(
        positions,
        triangles,
        6,
        partition_local_count=16,
        partition_local_target_faces=8,
    )
    assert partition_faces.shape[0] <= 6
    assert partition_stats["partition_local_count"] == 16
    assert partition_stats["global_cleanup_input_faces"] <= triangles.shape[0]
    assert partition_stats["target_reached"]

    try:
        meshutil.simplify(
            positions,
            triangles,
            6,
            partition_local_count=16,
            partition_local_target_faces=8,
            partition_local_max_epochs=0,
        )
    except ValueError:
        pass
    else:
        raise AssertionError("zero partition_local_max_epochs should be rejected")

    try:
        meshutil.simplify(positions.astype(np.float64), triangles, 6)
    except TypeError:
        pass
    else:
        raise AssertionError("float64 positions should be rejected")

    try:
        meshutil.simplify(positions, triangles, 6, memory_mode="invalid")
    except ValueError:
        pass
    else:
        raise AssertionError("invalid memory mode should be rejected")


if __name__ == "__main__":
    main()
