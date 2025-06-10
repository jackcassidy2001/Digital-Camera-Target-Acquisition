% Load RGB image
rgb_image = imread('burger.bmp');

figure(1);
imshow(rgb_image);

% Extract color channels
red_channel = rgb_image(:,:,1);
green_channel = rgb_image(:,:,2);
blue_channel = rgb_image(:,:,3);

% Get image dimensions
HEIGHT = size(rgb_image, 1); % Height corresponds to the first dimension
WIDTH = size(rgb_image, 2);  % Width corresponds to the second dimension

% Create Bayer image 
bayer_image = zeros(HEIGHT, WIDTH); % Corrected dimensions

% R G R G
% G B G B
% R G R G

% Start at pixel 1, 1. Horizontal Stride, Vertical Stride = 2
bayer_image(1:2:end, 1:2:end) = red_channel(1:2:end, 1:2:end);  % Red
% Start at pixel 1, 2. Horizontal Stride, Vertical Stride = 2
bayer_image(1:2:end, 2:2:end) = green_channel(1:2:end, 2:2:end);  % Green
% Start at pixel 2, 1. Horizontal Stride, Vertical Stride = 2
bayer_image(2:2:end, 1:2:end) = green_channel(2:2:end, 1:2:end);  % Green
% Start at pixel 2, 2. Horizontal Stride, Vertical Stride = 2
bayer_image(2:2:end, 2:2:end) = blue_channel(2:2:end, 2:2:end);  % Blue

% Display Bayer image
figure(2);
imshow(bayer_image, []);


% Initialize RGB image
rgb_image = zeros(HEIGHT, WIDTH, 3, 'uint8');

% Perform CFA demosaicing
bayer_image = padarray(bayer_image, [2, 2], 'both');

% R G R G
% G B G B
% R G R G

for y = 3:size(bayer_image, 2) - 2
    for x = 3:size(bayer_image, 1) - 2
        % Determine the color channel of the current pixel based on its position in the Bayer pattern
        is_red = mod(x, 2) == 1 && mod(y, 2) == 1;  % Red pixels at odd (1,1) positions
        is_green = mod(x, 2) ~= mod(y, 2);  % Green pixels at even (0,0) and odd (1,1) positions
        is_blue = mod(x, 2) == 0 && mod(y, 2) == 0;  % Blue pixels at even (0,0) positions
        
        if is_red
            red_ch = bayer_image(x, y);
            grn_ch = (bayer_image(x + 1, y) + bayer_image(x - 1, y) + bayer_image(x, y + 1) + bayer_image(x, y - 1)) / 4;
            blue_ch = (bayer_image(x + 1, y + 1) + bayer_image(x - 1, y - 1) + bayer_image(x + 1, y - 1) + bayer_image(x - 1, y + 1)) / 4;
        elseif is_green
            grn_ch = bayer_image(x, y);
            if (mod(y, 2) == 1)
                blue_ch = (bayer_image(x, y + 1) + bayer_image(x, y - 1)) / 2;
                red_ch = (bayer_image(x - 1, y) + bayer_image(x + 1, y)) / 2;
            else
                blue_ch = (bayer_image(x - 1, y) + bayer_image(x + 1, y)) / 2;
                red_ch = (bayer_image(x, y + 1) + bayer_image(x, y - 1)) / 2;
            end
        elseif is_blue
            blue_ch = bayer_image(x, y);
            red_ch = (bayer_image(x + 1, y + 1) + bayer_image(x - 1, y - 1) + bayer_image(x + 1, y - 1) + bayer_image(x - 1, y + 1)) / 4;
            grn_ch = (bayer_image(x + 1, y) + bayer_image(x - 1, y) + bayer_image(x, y + 1) + bayer_image(x, y - 1)) / 4;
        end

        rgb_image(x - 2, y - 2, 1) = red_ch;
        rgb_image(x - 2, y - 2, 2) = grn_ch;
        rgb_image(x - 2, y - 2, 3) = blue_ch;
    end
end

% Display and save RGB image
figure(3);
imshow(rgb_image);
imwrite(rgb_image, 'rgb_demosaic.bmp')

% Initialize YCbCr image
ycbcr_image = zeros(HEIGHT, WIDTH, 3, 'uint8');

% Transformation matrix
T = [0.183 0.614 0.062; -0.101 -0.338 0.439; 0.439 -0.399 -0.040];

% Bias Vector
offset = [16; 128; 128];

% Convert RGB to YCbCr
for y = 1:HEIGHT
    for x = 1:WIDTH
        RGB = double(reshape(rgb_image(y, x, :), [], 1)); % Convert RGB to column vector
        YCbCr = uint8(T * RGB + offset);
        
        ycbcr_image(y, x, :) = YCbCr;
    end
end

% Perform chroma subsampling (4:4:4 to 4:2:2)
for y = 1:HEIGHT
    for x = 1:WIDTH
        if mod(x, 2) == 0
            ycbcr_image(y, x, 2) = ycbcr_image(y, x - 1, 2);
            ycbcr_image(y, x, 3) = ycbcr_image(y, x - 1, 3);
        end
    end
end

% Convert YCbCr to RGB
rgb_image = ycbcr2rgb(ycbcr_image);

%figure(4);
% Display the RGB image
%imshow(rgb_image);

% Save Bayer image
%imwrite(rgb_image, 'YCbCr_422.bmp');

