#include <iostream>
#include <string>

bool downgradeSpineAtlas(const std::string& inputAtlasPath, const std::string& outputDirPath);

int main(int argc, char* argv[]) {
	if (argc != 3) {
		std::cout << "Usage: " << argv[0] << " <input_atlas> <output_dir>" << std::endl;
		return 1;
	}
	return downgradeSpineAtlas(argv[1], argv[2]) ? 0 : 1;
}
