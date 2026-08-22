#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// Function to compute the Mandelbrot set for a given complex number
int mandelbrot(double x, double y, int max_iterations) {
    double zx = 0.0;
    double zy = 0.0;
    double cx = x;
    double cy = y;
    int iteration = 0;

    while (iteration < max_iterations) {
        // Calculate |z|^2 = zx^2 + zy^2
        double magnitude squared = zx * zx + zy * zy;

        // If |z|^2 > 4, the point escapes to infinity
        if (magnitude squared > 4.0) {
            return iteration;
        }

        // Update z = z^2 + c
        double new_zx = zx * zx - zy * zy + cx;
        double new_zy = 2.0 * zx * zy + cy;

        zx = new_zx;
        zy = new_zy;

        iteration++;
    }

    // If we reach max_iterations, the point is in the Mandelbrot set
    return max_iterations;
}

// Function to write a minimal JPEG file (baseline, no Huffman, no DPCM)
int write_jpeg(const unsigned char *image, int width, int height, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open file for writing\n");
        return 0;
    }

    // JPEG header (SOI)
    fputc(0xFF, fp);
    fputc(0xD8, fp);

    // Define image properties
    int components = 3;  // RGB
    int precision = 8;   // 8 bits per sample
    int horizontal_sampling = 1;
    int vertical_sampling = 1;

    // APP0 segment (JFIF)
    fputc(0xFF, fp);
    fputc(0xE0, fp);
    fputc(16, fp);  // Length of segment
    fputc('J', fp);
    fputc('F', fp);
    fputc('I', fp);
    fputc('F', fp);
    fputc(0, fp);
    fputc(1, fp);  // Version
    fputc(0, fp);
    fputc(0, fp);  // Density units
    fputc(1, fp);  // X density
    fputc(1, fp);  // Y density
    fputc(0, fp);
    fputc(0, fp);

    // Define image dimensions
    fputc(0xFF, fp);
    fputc(0xC0, fp);  // SOF0 (Start of Frame)
    fputc(17, fp);    // Length of segment
    fputc(components, fp);
    fputc(width & 0xFF, fp);
    fputc((width >> 8) & 0xFF, fp);
    fputc(height & 0xFF, fp);
    fputc((height >> 8) & 0xFF, fp);
    fputc(precision, fp);

    // Define sampling for each component
    for (int i = 0; i < components; i++) {
        fputc(horizontal_sampling, fp);
        fputc(vertical_sampling, fp);
    }

    // IDAT segment (Image data)
    fputc(0xFF, fp);
    fputc(0xDA, fp);  // Start of Scan
    fputc(0, fp);     // Length will be calculated later

    // Write image data (no Huffman coding, no DPCM)
    // For each row, write the pixel values directly
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int index = (y * width + x) * 3;
            fputc(image[index + 0], fp);  // Red
            fputc(image[index + 1], fp);  // Green
            fputc(image[index + 2], fp);  // Blue
        }
    }

    // Calculate length of IDAT segment
    int idat_length = height * width * 3;
    fputc((idat_length & 0xFF), fp);
    fputc((idat_length >> 8) & 0xFF, fp);

    // EOD segment (End of Data)
    fputc(0xFF, fp);
    fputc(0xD9, fp);  // EOD

    // Close file
    fclose(fp);

    return 1;
}

int main() {
    // Parameters for the Mandelbrot set visualization
    int width = 800;
    int height = 600;
    double x_min = -2.0;
    double x_max = 1.0;
    double y_min = -1.5;
    double y_max = 1.5;
    int max_iterations = 100;

    // Allocate memory for the image data (RGB format)
    unsigned char *image = malloc(width * height * 3);
    if (!image) {
        fprintf(stderr, "Failed to allocate memory for image\n");
        return 1;
    }

    // Generate the Mandelbrot set image
    for (int y = 0; y < height; y++) {
        double cy = y_min + (y_max - y_min) * y / (height - 1);
        for (int x = 0; x < width; x++) {
            double cx = x_min + (x_max - x_min) * x / (width - 1);
            int iterations = mandelbrot(cx, cy, max_iterations);

            // Map iterations to RGB color
            unsigned char r, g, b;

            if (iterations == max_iterations) {
                // In the Mandelbrot set - black
                r = 0;
                g = 0;
                b = 0;
            } else {
                // Escape - color based on iteration count
                double t = (double)iterations / max_iterations;

                // Blue to red gradient
                r = (unsigned char)(255 * t);
                g = (unsigned char)(128 * (1 - t));
                b = (unsigned char)(255 * (1 - t);
            }

            // Set pixel color in the image array
            int index = (y * width + x) * 3;
            image[index + 0] = r;  // Red
            image[index + 1] = g;  // Green
            image[index + 2] = b;  // Blue
        }
    }

    // Write the image to a JPEG file
    if (!write_jpeg(image, width, height, "mandelbrot.jpg")) {
        fprintf(stderr, "Failed to write JPEG file\n");
        free(image);
        return 1;
    }

    // Clean up
    free(image);

    printf("Mandelbrot set image saved as 'mandelbrot.jpg'\n");
    return 0;
}
